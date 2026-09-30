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
#include <time.h>

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define CHUNK_SIZE (2 * 1024 * 1024)
#define NUM_WORKERS 2

#define HASH_LOG 12
#define HASH_SIZE (1 << HASH_LOG)
#define _32KB (32u * 1024u)
static_assert(HASH_SIZE * sizeof(uint32_t) <= _32KB,
              "hash table must fit in L1d");

static inline size_t compress_bound(size_t in_len) {
    return in_len + in_len / 255 + 64;
}
static inline uint32_t hash32(uint32_t a) {
    return (a * 0x9E3779B1) >> (32 - HASH_LOG);
}

chunk *chunks;
uint32_t num_chunks;

static size_t fast_compress(const uint8_t *in, size_t in_len, uint8_t *out);
void* compress_worker(void* arg);
static void shm_write(shm_data *shm, int *slot, const void *data, size_t size);
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





static int producer(const char* shm_path, const char* file_path, int core_id) {
    int fd __attribute__((cleanup(cleanup_fd))) = -1;
    mmap_region_t file_map __attribute__((cleanup(cleanup_mmap_region))) = {
        NULL,
        0
    };
    int shm_fd   __attribute__((cleanup(cleanup_fd)))          = -1;
    mmap_region_t shm_map  __attribute__((cleanup(cleanup_mmap_region))) = {
        NULL,
        0
    };
    global_ptr_guard_t chunks_guard __attribute__((cleanup(cleanup_global_ptr))) = {
        NULL
    };
    arena_t comp_arena __attribute__((cleanup(cleanup_arena))) = {
        NULL,
        0,
        0
    };


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
        file_map.ptr = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (file_map.ptr == MAP_FAILED) {
            return 1;
        }
        file_map.len = file_size;
        madvise(file_map.ptr, file_size, MADV_SEQUENTIAL);
    }

    shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        return 1;
    }
    ftruncate(shm_fd, sizeof(shm_data));

    shm_map.ptr = (shm_data*)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map.ptr == MAP_FAILED) {
        return 1;
    }
    shm_map.len = sizeof(shm_data);

    shm_data *shm = (shm_data *)shm_map.ptr;
    atomic_init(&shm->slots[0].state, STATE_EMPTY);
    atomic_init(&shm->slots[1].state, STATE_EMPTY);

    int current_slot = 0;
    num_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    chunks = (chunk*)calloc(num_chunks ? num_chunks : 1, sizeof(chunk));
    if (!chunks) {
        return 1;
    }

    size_t arena_need = compress_bound(file_size) + (size_t)num_chunks * 128;
    if (arena_need > 0 && arena_init(&comp_arena, arena_need) != 0) {
        return 1;
    }

    for (uint32_t i = 0; i < num_chunks; i++) {
        size_t offset = i * CHUNK_SIZE;
        chunks[i].in_data  = (uint8_t *)((char *)file_map.ptr + offset);
        chunks[i].in_len = (offset + CHUNK_SIZE > file_size) ? (file_size - offset) : CHUNK_SIZE;
        chunks[i].out_data = (uint8_t*)arena_alloc(&comp_arena, compress_bound(chunks[i].in_len));
        if(!chunks[i].out_data) {
            return 1;
        }
        atomic_init(&chunks[i].state, 0);
    }

    shm_write(shm, &current_slot, &num_chunks, sizeof(num_chunks));

    pthread_t tids[NUM_WORKERS];
    int wargs[NUM_WORKERS][2];
    for (int i = 0; i < NUM_WORKERS; i++) {
        wargs[i][0] = get_worker_cpu_core(core_id, i);
        wargs[i][1] = i;
        pthread_create(&tids[i], NULL, compress_worker, wargs[i]);
    }

    for (uint32_t i = 0; i < num_chunks; i++) {
        while (atomic_load_explicit(&chunks[i].state, memory_order_acquire) == 0) {
            cpu_relax();
        }

        chunk_hdr_t hdr = {
            .cs = (uint32_t)chunks[i].out_len,
            .us = (uint32_t)chunks[i].in_len
        };

        shm_write(shm, &current_slot, &hdr, sizeof(hdr));
        shm_write(shm, &current_slot, chunks[i].out_data, hdr.cs);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }

    while (atomic_load_explicit(&shm->slots[current_slot].state, memory_order_acquire) != STATE_EMPTY) {
        cpu_relax();
    }
    atomic_store_explicit(&shm->slots[current_slot].state, STATE_EOF, memory_order_release);

    return 0;
}

static size_t fast_compress(const uint8_t *in, size_t in_len, uint8_t *out) {
    static __thread uint32_t htab[HASH_SIZE];
    memset(htab, 0, sizeof(htab));

    if (__builtin_expect(in_len == 0, 0)) {
        out[0] = 0;
        return 1;
    }

    size_t ip = 0;
    size_t op = 0;
    size_t anchor = 0;
    size_t step = 1;
    size_t search_count = 0;

    while (ip + 8 < in_len) {
        __builtin_prefetch(in + ip + 64, 0, 0);
        uint32_t seq = *(uint32_t*)(in + ip);
        uint32_t h = hash32(seq);
        size_t ref = htab[h];
        htab[h] = ip;

        if (unlikely(ref > 0 && ip - ref < 65535 && *(uint32_t*)(in + ref) == seq)) {
            size_t lit_len = ip - anchor;
            size_t token_pos = op;
            if (lit_len >= 15) {
                out[op++] = (15 << 4);
                size_t l = lit_len - 15;
                while (l >= 255) { out[op++] = 255; l -= 255; }
                out[op++] = (uint8_t)l;
            } else {
                out[op++] = (uint8_t)(lit_len << 4);
            }
            memcpy(out + op, in + anchor, lit_len);
            op += lit_len;

            size_t match_len = 4;
            int mismatch_done = 0;
            while (ip + match_len + 8 <= in_len) {
                uint64_t a = *(const uint64_t *)(in + ip  + match_len);
                uint64_t b = *(const uint64_t *)(in + ref + match_len);
                uint64_t d = a ^ b;
                if (d == 0) {
                    match_len += 8;
                } else {
                    match_len += (unsigned)__builtin_ctzll(d) >> 3;
                    mismatch_done = 1;
                    break;
                }
            }
            if (!mismatch_done) {
                while (ip + match_len < in_len && in[ip + match_len] == in[ref + match_len]) {
                    match_len++;
                }
            }

            *(uint16_t*)(out + op) = (uint16_t)(ip - ref);
            op += 2;

            size_t ml = match_len - 4;
            if (ml >= 15) {
                out[token_pos] |= 15;
                ml -= 15;
                while (ml >= 255) { out[op++] = 255; ml -= 255; }
                out[op++] = (uint8_t)ml;
            } else {
                out[token_pos] |= (uint8_t)ml;
            }

            ip += match_len;
            anchor = ip;
            step = 1;
            search_count = 0;
        } else {
            ip += step;
            search_count++;
            step = 1 + (search_count >> 6);
        }
    }

    size_t lit_len = in_len - anchor;
    if (lit_len >= 15) {
        out[op++] = (15 << 4);
        size_t l = lit_len - 15;
        while (l >= 255) { out[op++] = 255; l -= 255; }
        out[op++] = (uint8_t)l;
    } else {
        out[op++] = (uint8_t)(lit_len << 4);
    }
    memcpy(out + op, in + anchor, lit_len);
    op += lit_len;
    return op;
}

void* compress_worker(void* arg) {
    const int *a = (const int *)arg;
    int core = a[0];
    int id   = a[1];
    pin_current_thread(core);

     for (uint32_t i = (uint32_t)id; i < num_chunks; i += NUM_WORKERS) {
        chunks[i].out_len = fast_compress(chunks[i].in_data, chunks[i].in_len, chunks[i].out_data);
        atomic_store_explicit(&chunks[i].state, 1, memory_order_release);
    }
    return NULL;
}

static void shm_write(shm_data *shm, int *slot, const void *data, size_t size) {
    size_t offset = 0;
    const char *ptr = (const char *)data;
    while (offset < size) {
        while (atomic_load_explicit(&shm->slots[*slot].state, memory_order_acquire) != STATE_EMPTY) cpu_relax();

        size_t chunk = size - offset;
        if (chunk > SHM_DATA_SIZE) chunk = SHM_DATA_SIZE;

        memcpy(shm->slots[*slot].data, ptr + offset, chunk);
        atomic_store_explicit(&shm->slots[*slot].wr_bytes, chunk, memory_order_relaxed);
        atomic_store_explicit(&shm->slots[*slot].state, STATE_FULL, memory_order_release);

        offset += chunk;
        *slot = 1 - *slot;
    }
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
