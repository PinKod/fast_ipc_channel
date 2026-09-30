#include "./../../include/cpu_utils.h"
#include "shm.h"

#include <stdio.h>
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

static int consumer(const char* shm_path, const char* file_path) __attribute__((hot));
static int out_file(char* output_file_buffer, const char* file_path);
static void reader_body(shm_ring* ring, int lane_id, char* out_mapping) __attribute__((hot, flatten));
void* reader_thread(void* arg) __attribute__((hot));

typedef struct {
    shm_ring* ring;
    char* out_mapping;
    int lane_id;
    int target_core;
    _Atomic int* ready_counter;
} reader_thread_ctx;

typedef struct {
    int fd;
    char* out_mapping;
    size_t offset;
    size_t len;
    int target_core;
} writeout_ctx;




int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(consumer(shm_path, file_path) != 0) return 1;

    return 0;
}




static void reader_body(shm_ring* ring, int lane_id, char* out_mapping) {
    shm_data* shm = &ring->lanes[lane_id];
    size_t chunk_index = (size_t)lane_id;

    while (1) {
        int64_t ctrl;
        int state;
        while ((state = SHM_UNPACK_STATE(ctrl = atomic_load_explicit(&shm->ctrl, memory_order_acquire))) == STATE_EMPTY) {
            cpu_relax();
        }

        if (unlikely(state == STATE_EOF)) {
            break;
        }

        int size = SHM_UNPACK_SIZE(ctrl);
        size_t file_offset = chunk_index * SHM_DATA_SIZE;
        memcpy(out_mapping + file_offset, shm->data, (size_t)size);
        atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_EMPTY, 0), memory_order_release);

        chunk_index += SHM_LANES;
    }
}

void* reader_thread(void* arg) {
    reader_thread_ctx* rctx = (reader_thread_ctx*)arg;
    pin_current_thread(rctx->target_core);
    unfair_sched();
    atomic_fetch_add_explicit(rctx->ready_counter, 1, memory_order_release);
    reader_body(rctx->ring, rctx->lane_id, rctx->out_mapping);
    return NULL;
}

// v17: hugetlb out_mapping (like v15) but flushed via parallel per-lane pwrite, not v15's single serial write
static void* writeout_thread(void* arg) {
    writeout_ctx* ctx = (writeout_ctx*)arg;
    if (ctx->target_core >= 0) {
        pin_current_thread(ctx->target_core);
    }

    size_t written = 0;
    while (written < ctx->len) {
        ssize_t n = pwrite(ctx->fd, ctx->out_mapping + ctx->offset + written, ctx->len - written, (off_t)(ctx->offset + written));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += (size_t)n;
    }
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

    char* out_mapping = NULL;
    if (file_size > 0) {
        out_mapping = (char *)mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if(unlikely(out_mapping == MAP_FAILED)) {
            perror("consumer: mmap(MAP_HUGETLB) failed - is a hugetlb pool reserved? "
                   "check /proc/meminfo HugePages_Total");
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

    _Atomic int readers_ready = 0;
    reader_thread_ctx rctxs[SHM_LANES - 1];
    pthread_t reader_tids[SHM_LANES - 1];
    for (int i = 0; i < SHM_LANES - 1; i++) {
        rctxs[i].ring = ring;
        rctxs[i].out_mapping = out_mapping;
        rctxs[i].lane_id = i + 1;
        rctxs[i].target_core = extra_reader_cores[i];
        rctxs[i].ready_counter = &readers_ready;
        pthread_create(&reader_tids[i], NULL, reader_thread, &rctxs[i]);
    }

    while (atomic_load_explicit(&readers_ready, memory_order_acquire) < SHM_LANES - 1) {
        cpu_relax();
    }
    unfair_sched();

    reader_body(ring, 0, out_mapping);

    for (int i = 0; i < SHM_LANES - 1; i++) {
        pthread_join(reader_tids[i], NULL);
    }

    if (out_mapping) {
        size_t quarter = file_size / SHM_LANES;
        writeout_ctx wctxs[SHM_LANES - 1];
        pthread_t writeout_tids[SHM_LANES - 1];
        for (int i = 0; i < SHM_LANES - 1; i++) {
            wctxs[i].fd = fd;
            wctxs[i].out_mapping = out_mapping;
            wctxs[i].offset = (size_t)(i + 1) * quarter;
            wctxs[i].len = quarter;
            wctxs[i].target_core = extra_reader_cores[i];
            pthread_create(&writeout_tids[i], NULL, writeout_thread, &wctxs[i]);
        }

        writeout_ctx wctx0 = { .fd = fd, .out_mapping = out_mapping, .offset = 0,
                                .len = file_size - (size_t)(SHM_LANES - 1) * quarter, .target_core = -1 };
        writeout_thread(&wctx0);

        for (int i = 0; i < SHM_LANES - 1; i++) {
            pthread_join(writeout_tids[i], NULL);
        }
    }

    if(out_mapping) munmap(out_mapping, file_size);
    munmap(ring, sizeof(shm_ring));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);
    return 0;
}
