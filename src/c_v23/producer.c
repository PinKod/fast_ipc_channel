#define _DEFAULT_SOURCE
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
#include <stdlib.h>
#include <pthread.h>


#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#elif defined(__aarch64__)
    #define cpu_relax() __asm__ volatile("yield\n": : :"memory")
#else
    #define cpu_relax() do {} while(0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static const int extra_worker_cores[SHM_LANES - 1] = { 10, 11, 1 };

typedef struct {
    shm_ring* ring;
    const char* file_mapping;
    size_t file_size;
    int lane_id;
    int target_core;
    char* xor_buf;
    char* comp_buf;
    size_t comp_buf_cap;
    _Atomic int* ready_counter;
    _Atomic uint64_t* total_uncompressed;
    _Atomic uint64_t* total_compressed;
} worker_ctx;

static int producer(const char* shm_path, const char* file_path, int core_id) __attribute__((hot));
static void worker_body(worker_ctx* ctx) __attribute__((hot, flatten));
void* worker_thread(void* arg) __attribute__((hot));




int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(producer(shm_path, file_path, core_id) != 0) return 1;

    return 0;
}


// c_v23: XOR-delta encode before LZ4 replaces c_v22_3's plain byte-level LZ4 compression
static void xor_delta_encode(const char* records, size_t count, char* out_xor) {
    uint32_t prev = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t val;
        memcpy(&val, records + i * RECORD_SIZE, 4);
        uint32_t delta = val ^ prev;
        memcpy(out_xor + i * 4, &delta, 4);
        prev = val;
    }
}

static void send_block(shm_data* shm, uint32_t block_id, uint32_t record_count,
                        const char* payload, size_t payload_len) {
    lz4_block_header hdr = {
        .block_id = block_id,
        .comp_size = (uint32_t)payload_len,
        .record_count = record_count
    };

    while (SHM_UNPACK_STATE(atomic_load_explicit(&shm->ctrl, memory_order_acquire)) != STATE_EMPTY) {
        cpu_relax();
    }
    memcpy(shm->data, &hdr, sizeof(hdr));
    atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_BLOCK_HEADER, sizeof(hdr)), memory_order_release);

    size_t sent = 0;
    while (sent < payload_len) {
        size_t chunk_size = SHM_DATA_SIZE;
        if (unlikely(sent + chunk_size > payload_len)) {
            chunk_size = payload_len - sent;
        }

        while (SHM_UNPACK_STATE(atomic_load_explicit(&shm->ctrl, memory_order_acquire)) != STATE_EMPTY) {
            cpu_relax();
        }
        memcpy(shm->data, payload + sent, chunk_size);
        atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_FULL, chunk_size), memory_order_release);
        sent += chunk_size;
    }
}

static void worker_body(worker_ctx* ctx) {
    shm_data* shm = &ctx->ring->lanes[ctx->lane_id];

    char* xor_buf = ctx->xor_buf;
    char* comp_buf = ctx->comp_buf;
    size_t comp_buf_cap = ctx->comp_buf_cap;

    uint64_t local_uncompressed = 0;
    uint64_t local_compressed = 0;

    size_t full_records = ctx->file_size / RECORD_SIZE;
    size_t num_full_blocks = (full_records + RECORDS_PER_BLOCK - 1) / RECORDS_PER_BLOCK;
    size_t block_id = (size_t)ctx->lane_id;

    while (block_id < num_full_blocks) {
        size_t record_offset = block_id * RECORDS_PER_BLOCK;
        size_t record_count = RECORDS_PER_BLOCK;
        if (unlikely(record_offset + record_count > full_records)) {
            record_count = full_records - record_offset;
        }

        xor_delta_encode(ctx->file_mapping + record_offset * RECORD_SIZE, record_count, xor_buf);

        int comp_len = LZ4_compress_default(xor_buf, comp_buf, (int)(record_count * 4), (int)comp_buf_cap);
        if (unlikely(comp_len <= 0)) {
            fprintf(stderr, "producer: LZ4_compress_default failed on block %zu (lane %d)\n",
                    block_id, ctx->lane_id);
            exit(1);
        }

        send_block(shm, (uint32_t)block_id, (uint32_t)record_count, comp_buf, (size_t)comp_len);

        local_uncompressed += record_count * RECORD_SIZE;
        local_compressed += (uint64_t)comp_len;
        block_id += SHM_LANES;
    }

    if (ctx->lane_id == 0) {
        size_t tail_bytes = ctx->file_size - full_records * RECORD_SIZE;
        if (unlikely(tail_bytes > 0)) {
            send_block(shm, (uint32_t)num_full_blocks, 0,
                       ctx->file_mapping + full_records * RECORD_SIZE, tail_bytes);
            local_uncompressed += tail_bytes;
            local_compressed += tail_bytes;
        }
    }

    while (SHM_UNPACK_STATE(atomic_load_explicit(&shm->ctrl, memory_order_acquire)) != STATE_EMPTY) {
        cpu_relax();
    }
    atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_EOF, 0), memory_order_release);

    atomic_fetch_add_explicit(ctx->total_uncompressed, local_uncompressed, memory_order_relaxed);
    atomic_fetch_add_explicit(ctx->total_compressed, local_compressed, memory_order_relaxed);
}

void* worker_thread(void* arg) {
    worker_ctx* ctx = (worker_ctx*)arg;
    pin_current_thread(ctx->target_core);
    unfair_sched();
    atomic_fetch_add_explicit(ctx->ready_counter, 1, memory_order_release);
    worker_body(ctx);
    return NULL;
}

static int producer(const char* shm_path, const char* file_path, int core_id) {
    (void)core_id;
    int fd = open(file_path, O_RDONLY);
    if(fd == -1) return 1;

    struct stat st;
    if(unlikely(stat(file_path, &st)) == -1) {
        close(fd);
        return 1;
    }

    size_t file_size = st.st_size;
    char* file_mapping = NULL;
    if (file_size > 0) {
        file_mapping = (char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (file_mapping == MAP_FAILED) {
            close(fd);
            return 1;
        }
        madvise(file_mapping, file_size, MADV_SEQUENTIAL);
        madvise(file_mapping, file_size, MADV_WILLNEED);
    }

    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if(unlikely(shm_fd == -1)) {
        if(file_mapping) munmap(file_mapping, file_size);
        close(fd);
        return 1;
    }

    ftruncate(shm_fd, sizeof(shm_ring));
    shm_ring *ring = (shm_ring *)mmap(NULL, sizeof(shm_ring), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(unlikely(ring == MAP_FAILED)) {
        if(file_mapping) munmap(file_mapping, file_size);
        close(fd);
        close(shm_fd);
        return 1;
    }

    for (int lane = 0; lane < SHM_LANES; lane++) {
        atomic_init(&ring->lanes[lane].ctrl, SHM_PACK(STATE_EMPTY, 0));
    }

    size_t xor_buf_cap = BLOCK_XOR_BYTES;
    size_t comp_buf_cap = (size_t)LZ4_compressBound(BLOCK_XOR_BYTES);
    size_t lane_stride = xor_buf_cap + comp_buf_cap;
    size_t bufs_needed = lane_stride * SHM_LANES;

    char* bufs = (char *)malloc(bufs_needed);
    if (unlikely(!bufs)) {
        fprintf(stderr, "producer: OOM allocating XOR/LZ4 buffers\n");
        if(file_mapping) munmap(file_mapping, file_size);
        munmap(ring, sizeof(shm_ring));
        close(fd);
        close(shm_fd);
        return 1;
    }

    _Atomic uint64_t total_uncompressed = 0;
    _Atomic uint64_t total_compressed = 0;
    _Atomic int workers_ready = 0;

    worker_ctx ctxs[SHM_LANES];
    for (int i = 0; i < SHM_LANES; i++) {
        ctxs[i].ring = ring;
        ctxs[i].file_mapping = file_mapping;
        ctxs[i].file_size = file_size;
        ctxs[i].lane_id = i;
        ctxs[i].target_core = 0;
        ctxs[i].xor_buf = bufs + (size_t)i * lane_stride;
        ctxs[i].comp_buf = bufs + (size_t)i * lane_stride + xor_buf_cap;
        ctxs[i].comp_buf_cap = comp_buf_cap;
        ctxs[i].ready_counter = &workers_ready;
        ctxs[i].total_uncompressed = &total_uncompressed;
        ctxs[i].total_compressed = &total_compressed;
    }

    pthread_t tids[SHM_LANES - 1];
    for (int i = 1; i < SHM_LANES; i++) {
        ctxs[i].target_core = extra_worker_cores[i - 1];
        pthread_create(&tids[i - 1], NULL, worker_thread, &ctxs[i]);
    }

    while (atomic_load_explicit(&workers_ready, memory_order_acquire) < SHM_LANES - 1) {
        cpu_relax();
    }
    unfair_sched();

    worker_body(&ctxs[0]);

    for (int i = 0; i < SHM_LANES - 1; i++) {
        pthread_join(tids[i], NULL);
    }

    double ratio_pct = total_uncompressed > 0
        ? (1.0 - (double)total_compressed / (double)total_uncompressed) * 100.0
        : 0.0;
    printf("Producer: %llu bytes in, %llu bytes compressed, ratio %.2f%%\n",
           (unsigned long long)total_uncompressed, (unsigned long long)total_compressed, ratio_pct);

    free(bufs);
    if(file_mapping) {
        munmap(file_mapping, file_size);
    }
    munmap(ring, sizeof(shm_ring));
    close(fd);
    close(shm_fd);
    return 0;
}
