#define _DEFAULT_SOURCE
#include "./../../include/cpu_utils.h"
#include "shm.h"
#include "cleanup.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define BLOCK_FLOATS 64
#define NUM_WORKERS 2

static block_t *blocks;
static uint32_t num_blocks;
static char *file_map;



static int compress_block(const uint32_t* in, int count, uint8_t* out);
void* compress_worker(void* arg);
static int producer(const char* shm_path, const char* file_path, int core_id);
static int get_worker_cpu_core(int main_core, int worker_idx);




int main(int argc, char* argv[]) {
    if (argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);

    if (pin_current_thread(core_id) != 0) return 1;
    if (producer(shm_path, file_path, core_id) != 0) return 1;

    return 0;
}


static int compress_block(const uint32_t* in, int count, uint8_t* out) {
    size_t op = 0;
    uint32_t prev = 0;
    out[op++] = (uint8_t)count;

    size_t start_op = op;
    for (int i = 0; i < count; i++) {
        uint32_t val = in[i];
        uint32_t diff = val ^ prev;
        prev = val;

        if (diff == 0) {
            out[op++] = 0;
        } else if ((diff & 0xFFFFFF00) == 0) {
            out[op++] = 1; out[op++] = (uint8_t)diff;
        } else if ((diff & 0xFFFF0000) == 0) {
            out[op++] = 2; out[op++] = (uint8_t)diff; out[op++] = (uint8_t)(diff >> 8);
        } else if ((diff & 0xFF000000) == 0) {
            out[op++] = 3; out[op++] = (uint8_t)diff; out[op++] = (uint8_t)(diff >> 8); out[op++] = (uint8_t)(diff >> 16);
        } else {
            out[op++] = 4;
            *(uint32_t*)(out + op) = diff;
            op += 4;
        }
    }

    if ((op - start_op) >= (size_t)(count * 4)) {
        out[0] = (uint8_t)(count | 0x80);
        memcpy(out + 1, in, count * 4);
        return 1 + count * 4;
    }
    return op;
}

void* compress_worker(void* arg) {
    const int *a = (const int *)arg;
    pin_current_thread(a[0]);
    int id = a[1];

    for (uint32_t b = (uint32_t)id; b < num_blocks; b += NUM_WORKERS) {
        uint32_t floats[64];
        int count = (b == num_blocks - 1 && (num_blocks * BLOCK_FLOATS > (num_blocks * BLOCK_FLOATS))) ? BLOCK_FLOATS : BLOCK_FLOATS;


        size_t offset = b * BLOCK_FLOATS * 5;
        for (int i = 0; i < 64; i++) {
            memcpy(&floats[i], file_map + offset + i * 5, 4);
        }

        blocks[b].len = compress_block(floats, 64, blocks[b].data);
        atomic_store_explicit(&blocks[b].state, 1, memory_order_release);
    }
    return NULL;
}

static int producer(const char* shm_path, const char* file_path, int core_id) {
    int fd __attribute__((cleanup(cleanup_fd))) = -1;
    mmap_region_t file_map_reg __attribute__((cleanup(cleanup_mmap_region))) = {NULL, 0};
    int shm_fd __attribute__((cleanup(cleanup_fd))) = -1;
    mmap_region_t shm_map_reg __attribute__((cleanup(cleanup_mmap_region))) = {NULL, 0};
    global_ptr_guard_t blocks_guard __attribute__((cleanup(cleanup_global_ptr))) = {NULL};

    fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        return 1;
    }
    size_t file_size = st.st_size;

    if (file_size > 0) {
        file_map_reg.ptr = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (file_map_reg.ptr == MAP_FAILED) {
            return 1;
        }
        file_map_reg.len = file_size;
        file_map = (char*)file_map_reg.ptr;
    }

    shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        return 1;
    }
    ftruncate(shm_fd, sizeof(shm_data));

    shm_map_reg.ptr = mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map_reg.ptr == MAP_FAILED) {
        return 1;
    }
    shm_map_reg.len = sizeof(shm_data);

    shm_data *shm = (shm_data *)shm_map_reg.ptr;
    atomic_init(&shm->head, 0);
    atomic_init(&shm->tail, 0);

    uint32_t total_floats = file_size / 5;
    num_blocks = (total_floats + BLOCK_FLOATS - 1) / BLOCK_FLOATS;

    blocks = (block_t*)calloc(num_blocks ? num_blocks : 1, sizeof(block_t));
    if (!blocks) {
        return 1;
    }
    blocks_guard.ref = (void**)&blocks;

    for (uint32_t i = 0; i < num_blocks; i++) atomic_init(&blocks[i].state, 0);

    shm_send(shm, &total_floats, 4);

    pthread_t tids[NUM_WORKERS];
    int wargs[NUM_WORKERS][2];
    for (int i = 0; i < NUM_WORKERS; i++) {
        wargs[i][0] = get_worker_cpu_core(core_id, i);
        wargs[i][1] = i;
        pthread_create(&tids[i], NULL, compress_worker, wargs[i]);
    }

    uint32_t floats_processed = 0;
    for (uint32_t i = 0; i < num_blocks; i++) {
        while (atomic_load_explicit(&blocks[i].state, memory_order_acquire) == 0) {
            cpu_relax();
        }

        uint32_t count = (floats_processed + BLOCK_FLOATS <= total_floats) ? BLOCK_FLOATS : (total_floats - floats_processed);

        if (count < BLOCK_FLOATS) {
            uint32_t floats[64];
            size_t offset = i * BLOCK_FLOATS * 5;
            for (uint32_t j = 0; j < count; j++) {
                memcpy(&floats[j], file_map + offset + j * 5, 4);
            }
            blocks[i].len = compress_block(floats, count, blocks[i].data);
        }

        uint32_t msg_len = blocks[i].len;
        shm_send(shm, &msg_len, 4);
        shm_send(shm, blocks[i].data, msg_len);

        floats_processed += count;
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }

    uint32_t eof = 0;
    shm_send(shm, &eof, 4);

    return 0;
}

static int get_worker_cpu_core(int main_core, int worker_idx) {
    if (main_core == 0) return (worker_idx == 0) ? 1 : 3;
    if (main_core == 2) return (worker_idx == 0) ? 5 : 7;
    static const int table[4][2] = {
        {1, 3},
        {0, 2},
        {1, 3},
        {0, 2},
    };
    return table[main_core & 3][worker_idx];
}
