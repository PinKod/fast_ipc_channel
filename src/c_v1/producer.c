#define _DEFAULT_SOURCE
#include "./../include/cpu_utils.h"
#include "shm.h"

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

#define CHUNK_SIZE (2 * 1024 * 1024)
#define NUM_WORKERS 2

#define HASH_LOG 14
#define HASH_SIZE (1 << HASH_LOG)

static inline uint32_t hash32(uint32_t a) { 
    return (a * 0x9E3779B1) >> (32 - HASH_LOG); 
}

chunk *chunks;
uint32_t num_chunks;
_Atomic int current_worker_idx = 0;

static size_t fast_compress(const uint8_t *in, size_t in_len, uint8_t *out);
void* compress_worker(void* arg);
static void shm_write(shm_data *shm, int *slot, const void *data, size_t size);
static int producer(const char* shm_path, const char* file_path, int core_id);

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
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) return 1;
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return 1;
    }
    size_t file_size = st.st_size;

    char* file_mapping = NULL;
    if (file_size > 0) {
        file_mapping = (char*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (file_mapping == MAP_FAILED) {
            close(fd);
            return 1;
        }
        madvise(file_mapping, file_size, MADV_SEQUENTIAL);
    }

    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        if (file_mapping) munmap(file_mapping, file_size);
        close(fd);
        return 1;
    }
    ftruncate(shm_fd, sizeof(shm_data));
    
    shm_data *shm = (shm_data*)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        if (file_mapping) munmap(file_mapping, file_size);
        close(fd);
        close(shm_fd);
        return 1;
    }
    
    atomic_init(&shm->slots[0].state, STATE_EMPTY);
    atomic_init(&shm->slots[1].state, STATE_EMPTY);

    int current_slot = 0;
    num_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    chunks = (chunk*)calloc(num_chunks, sizeof(chunk));
    
    for (uint32_t i = 0; i < num_chunks; i++) {
        size_t offset = i * CHUNK_SIZE;
        chunks[i].in_data = (uint8_t*)(file_mapping + offset);
        chunks[i].in_len = (offset + CHUNK_SIZE > file_size) ? (file_size - offset) : CHUNK_SIZE;
        chunks[i].out_data = (uint8_t*)malloc(chunks[i].in_len + 1024);
        atomic_init(&chunks[i].state, 0);
    }

    shm_write(shm, &current_slot, &num_chunks, sizeof(num_chunks));
    pthread_t tids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        int *core = (int*)malloc(sizeof(int));
        *core = (core_id + 2 * (i + 1)) % 8;
        pthread_create(&tids[i], NULL, compress_worker, core);
    }
    for (uint32_t i = 0; i < num_chunks; i++) {
        while (atomic_load_explicit(&chunks[i].state, memory_order_acquire) == 0) cpu_relax();
        
        uint32_t cs = chunks[i].out_len;
        uint32_t us = chunks[i].in_len;
        
        shm_write(shm, &current_slot, &cs, sizeof(cs));
        shm_write(shm, &current_slot, &us, sizeof(us));
        shm_write(shm, &current_slot, chunks[i].out_data, cs);
        
        free(chunks[i].out_data);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }
    
    while (atomic_load_explicit(&shm->slots[current_slot].state, memory_order_acquire) != STATE_EMPTY) cpu_relax();
    atomic_store_explicit(&shm->slots[current_slot].state, STATE_EOF, memory_order_release);

    free(chunks);
    if (file_mapping) munmap(file_mapping, file_size);
    close(fd);
    munmap(shm, sizeof(shm_data));
    close(shm_fd);
    return 0;
}

static size_t fast_compress(const uint8_t *in, size_t in_len, uint8_t *out) {
    uint32_t htab[HASH_SIZE] = {0};
    size_t ip = 0, op = 0, anchor = 0;

    while (ip + 8 < in_len) {
        uint32_t seq = *(uint32_t*)(in + ip);
        uint32_t h = hash32(seq);
        size_t ref = htab[h];
        htab[h] = ip;

        if (ref > 0 && ip - ref < 65535 && *(uint32_t*)(in + ref) == seq) {
            size_t lit_len = ip - anchor;
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
            while (ip + match_len < in_len && in[ip + match_len] == in[ref + match_len]) match_len++;

            *(uint16_t*)(out + op) = (uint16_t)(ip - ref); 
            op += 2;

            size_t ml = match_len - 4;
            if (ml >= 15) {
                out[op - 3] |= 15;
                ml -= 15;
                while (ml >= 255) { out[op++] = 255; ml -= 255; }
                out[op++] = (uint8_t)ml;
            } else {
                out[op - 3] |= (uint8_t)ml;
            }

            ip += match_len;
            anchor = ip;
        } else {
            ip++;
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
    int core = *(int*)arg;
    pin_current_thread(core);
    free(arg);

    while (1) {
        int idx = atomic_fetch_add_explicit(&current_worker_idx, 1, memory_order_relaxed);
        if (idx >= (int)num_chunks) break;

        chunks[idx].out_len = fast_compress(chunks[idx].in_data, chunks[idx].in_len, chunks[idx].out_data);
        atomic_store_explicit(&chunks[idx].state, 1, memory_order_release);
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