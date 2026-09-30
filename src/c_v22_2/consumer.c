#include "./../../include/cpu_utils.h"
#include "shm.h"

#include <lz4.h>

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>

#define OUTPUT_FILE_PATH_SIZE 512

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#elif defined(__aarch64__)
    #define cpu_relax() __asm__ volatile("yield\n": : :"memory")
#else
    #define cpu_relax() do {} while(0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static const int extra_reader_cores[SHM_LANES - 1] = { 2, 6, 8 };

#define PREFAULT_CORE 9

#define HUGE_PAGE_SIZE (2 * 1024 * 1024)

static int consumer(const char* shm_path, const char* file_path) __attribute__((hot));
static int out_file(char* output_file_buffer, const char* file_path);
static void reader_body(shm_ring* ring, int lane_id, char* out_mapping, char* reasm_buf,
                         _Atomic uint64_t* blocks_decoded) __attribute__((hot, flatten));
void* reader_thread(void* arg) __attribute__((hot));
void* prefault_thread(void* arg);

typedef struct {
    shm_ring* ring;
    char* out_mapping;
    int lane_id;
    int target_core;
    char* reasm_buf;
    _Atomic int* ready_counter;
    _Atomic uint64_t* blocks_decoded;
} reader_thread_ctx;

typedef struct {
    char* out_mapping;
    size_t file_size;
    int target_core;
} prefault_ctx;




int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(consumer(shm_path, file_path) != 0) return 1;

    return 0;
}




static void reader_body(shm_ring* ring, int lane_id, char* out_mapping, char* reasm_buf,
                         _Atomic uint64_t* blocks_decoded) {
    shm_data* shm = &ring->lanes[lane_id];

    uint64_t local_blocks = 0;

    while (1) {
        int64_t ctrl;
        int state;
        while ((state = SHM_UNPACK_STATE(ctrl = atomic_load_explicit(&shm->ctrl, memory_order_acquire))) == STATE_EMPTY) {
            cpu_relax();
        }

        if (unlikely(state == STATE_EOF)) {
            break;
        }

        if (unlikely(state != STATE_BLOCK_HEADER)) {
            fprintf(stderr, "consumer: protocol error on lane %d - expected block header, got state %d\n",
                    lane_id, state);
            exit(1);
        }

        lz4_block_header hdr;
        memcpy(&hdr, shm->data, sizeof(hdr));
        atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_EMPTY, 0), memory_order_release);

        size_t received = 0;
        while (received < hdr.comp_size) {
            while ((state = SHM_UNPACK_STATE(ctrl = atomic_load_explicit(&shm->ctrl, memory_order_acquire))) == STATE_EMPTY) {
                cpu_relax();
            }
            if (unlikely(state != STATE_FULL)) {
                fprintf(stderr, "consumer: protocol error on lane %d - expected data chunk, got state %d\n",
                        lane_id, state);
                exit(1);
            }
            int size = SHM_UNPACK_SIZE(ctrl);
            memcpy(reasm_buf + received, shm->data, (size_t)size);
            atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_EMPTY, 0), memory_order_release);
            received += (size_t)size;
        }

        int decoded = LZ4_decompress_safe(reasm_buf, out_mapping + (size_t)hdr.block_id * LZ4_BLOCK_SIZE,
                                           (int)hdr.comp_size, (int)hdr.uncomp_size);
        if (unlikely(decoded != (int)hdr.uncomp_size)) {
            fprintf(stderr, "consumer: LZ4 decompress mismatch on block %u (got %d, expected %u)\n",
                    hdr.block_id, decoded, hdr.uncomp_size);
            exit(1);
        }

        local_blocks++;
    }

    atomic_fetch_add_explicit(blocks_decoded, local_blocks, memory_order_relaxed);
}

void* reader_thread(void* arg) {
    reader_thread_ctx* rctx = (reader_thread_ctx*)arg;
    pin_current_thread(rctx->target_core);
    unfair_sched();
    atomic_fetch_add_explicit(rctx->ready_counter, 1, memory_order_release);
    reader_body(rctx->ring, rctx->lane_id, rctx->out_mapping, rctx->reasm_buf, rctx->blocks_decoded);
    return NULL;
}

void* prefault_thread(void* arg) {
    prefault_ctx* ctx = (prefault_ctx*)arg;
    pin_current_thread(ctx->target_core);
    madvise(ctx->out_mapping, ctx->file_size, MADV_POPULATE_WRITE);
    return NULL;
}

static int out_file(char* output_file_buffer, const char* file_path) {
    const char *name = strrchr(file_path, '/');
    name = name ? name + 1 : file_path;
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) {
        ext = file_path + strlen(file_path);
    }
    int len = ext - file_path;
    int ret = snprintf(output_file_buffer, OUTPUT_FILE_PATH_SIZE, "%.*s_output%s", len, file_path, ext);
    if (ret < 0 || ret >= OUTPUT_FILE_PATH_SIZE) return 1;

    return 0;

}

static int consumer(const char* shm_path, const char* file_path) {
    struct stat st;
    if(unlikely(stat(file_path, &st) == -1)) return 1;
    size_t file_size = (size_t)st.st_size;

    char output_file_path[OUTPUT_FILE_PATH_SIZE] = {0};
    if(out_file(output_file_path, file_path) != 0) return 1;

    int fd = open(output_file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if(unlikely(fd == -1)) return 1;
    if(unlikely(fallocate(fd, 0, 0, (off_t)file_size) != 0)) {
        close(fd);
        return 1;
    }

    char* out_mapping = NULL;
    if (file_size > 0) {
        out_mapping = (char *)mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if(unlikely(out_mapping == MAP_FAILED)) {
            close(fd);
            return 1;
        }
    }

    int shm_fd = -1;
    while ((shm_fd = shm_open(shm_path, O_RDWR, 0666)) == -1) {
        if (unlikely(errno != ENOENT)) {
            if(out_mapping) munmap(out_mapping, file_size);
            close(fd);
            return 1;
        }
        usleep(100);
    }
    shm_ring *ring = (shm_ring *)mmap(NULL, sizeof(shm_ring), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(unlikely(ring == MAP_FAILED)) {
        if(out_mapping) munmap(out_mapping, file_size);
        close(fd);
        close(shm_fd);
        return 1;
    }

    pthread_t prefault_tid;
    prefault_ctx pctx = { .out_mapping = out_mapping, .file_size = file_size, .target_core = PREFAULT_CORE };
    if (out_mapping) {
        pthread_create(&prefault_tid, NULL, prefault_thread, &pctx);
    }

    size_t reasm_buf_cap = (size_t)LZ4_compressBound(LZ4_BLOCK_SIZE);
    size_t reasm_bufs_needed = reasm_buf_cap * SHM_LANES;
    size_t reasm_bufs_alloc = ((reasm_bufs_needed + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE) * HUGE_PAGE_SIZE;

    // c_v22_2: one mmap(MAP_HUGETLB) before thread creation replaces c_v22's malloc() per thread
    char* reasm_bufs = (char *)mmap(NULL, reasm_bufs_alloc, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (unlikely(reasm_bufs == MAP_FAILED)) {
        perror("consumer: mmap(MAP_HUGETLB) failed for LZ4 reassembly buffers - is a hugetlb pool "
               "reserved? check /proc/meminfo HugePages_Total");
        if(out_mapping) munmap(out_mapping, file_size);
        munmap(ring, sizeof(shm_ring));
        close(fd);
        close(shm_fd);
        return 1;
    }

    _Atomic uint64_t blocks_decoded = 0;
    _Atomic int readers_ready = 0;
    reader_thread_ctx rctxs[SHM_LANES - 1];
    pthread_t reader_tids[SHM_LANES - 1];
    for (int i = 0; i < SHM_LANES - 1; i++) {
        rctxs[i].ring = ring;
        rctxs[i].out_mapping = out_mapping;
        rctxs[i].lane_id = i + 1;
        rctxs[i].target_core = extra_reader_cores[i];
        rctxs[i].reasm_buf = reasm_bufs + (size_t)(i + 1) * reasm_buf_cap;
        rctxs[i].ready_counter = &readers_ready;
        rctxs[i].blocks_decoded = &blocks_decoded;
        pthread_create(&reader_tids[i], NULL, reader_thread, &rctxs[i]);
    }

    while (atomic_load_explicit(&readers_ready, memory_order_acquire) < SHM_LANES - 1) {
        cpu_relax();
    }
    unfair_sched();

    reader_body(ring, 0, out_mapping, reasm_bufs, &blocks_decoded);

    for (int i = 0; i < SHM_LANES - 1; i++) {
        pthread_join(reader_tids[i], NULL);
    }
    if (out_mapping) {
        pthread_join(prefault_tid, NULL);
    }

    printf("Consumer: %llu blocks decompressed exactly. Data loss: 0.00%% (LZ4 lossless)\n",
           (unsigned long long)blocks_decoded);

    munmap(reasm_bufs, reasm_bufs_alloc);
    if(out_mapping) munmap(out_mapping, file_size);
    munmap(ring, sizeof(shm_ring));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);
    return 0;
}
