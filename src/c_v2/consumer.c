#define _DEFAULT_SOURCE
#include "./../include/cpu_utils.h"
#include "shm.h"
#include "cleanup.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>

#define OUTPUT_FILE_PATH_SIZE 512
#define NUM_WORKERS 2

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#endif

chunk *chunks;
uint32_t num_chunks;

static void fast_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) __attribute__((hot, flatten));
static void shm_read(shm_data *shm, int *slot, void *dst, size_t size);
void* decompress_worker(void* arg);
void* write_worker(void* arg);
static int consumer(const char* shm_path, const char* file_path, int core_id);
static int out_file(char* output_file_buffer, const char* file_path);
static int get_worker_cpu_core(int main_core, int idx);





int main(int argc, char* argv[]) {
    if (argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    
    if (pin_current_thread(core_id) != 0) return 1;
    if (consumer(shm_path, file_path, core_id) != 0) return 1;

    return 0;
}





static void fast_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    size_t ip = 0;
    size_t op = 0;
    while (ip < in_len && op < out_len) {
        uint8_t token = in[ip++];
        size_t lit_len = token >> 4;
        if (lit_len == 15) {
            uint8_t l;
            do { 
                l = in[ip++];
                lit_len += l;
            } while (l == 255);
        }
        if (lit_len > 0) {
            memcpy(out + op, in + ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        if (ip >= in_len) break;

        size_t off = in[ip] | (in[ip+1] << 8);
        ip += 2;
        
        size_t match_len = (token & 15) + 4;
        if (match_len == 15 + 4) {
            uint8_t l;
            do { 
                l = in[ip++];
                match_len += l;
            } while (l == 255);
        }

        uint8_t *dst = out + op;
        const uint8_t *src = out + op - off;
        size_t remaining = match_len;
        if (off >= 8) {
            while (remaining >= 8) {
                *(uint64_t *)dst = *(const uint64_t *)src;
                dst += 8; src += 8; remaining -= 8;
            }
        }
        while (remaining--) {
            *dst++ = *src++;
        }
        op += match_len;
    }
}

static void shm_read(shm_data *shm, int *slot, void *dst, size_t size) {
    static char buf[SHM_SLOT_SIZE];
    static int buf_len = 0;
    static int buf_pos = 0;

    size_t offset = 0;
    char *ptr = (char *)dst;

    while (offset < size) {
        if (buf_pos < buf_len) {
            size_t take = buf_len - buf_pos;
            if (take > size - offset) take = size - offset;
            memcpy(ptr + offset, buf + buf_pos, take);
            buf_pos += take;
            offset += take;
        } else {
            int state;
            while ((state = atomic_load_explicit(&shm->slots[*slot].state, memory_order_acquire)) == STATE_EMPTY) cpu_relax();
            if (state == STATE_EOF) exit(0);
            
            buf_len = atomic_load_explicit(&shm->slots[*slot].wr_bytes, memory_order_relaxed);
            memcpy(buf, shm->slots[*slot].data, buf_len);
            
            atomic_store_explicit(&shm->slots[*slot].state, STATE_EMPTY, memory_order_release);
            *slot = 1 - *slot;
            buf_pos = 0;
        }
    }
}

void* decompress_worker(void* arg) {
    const int *a   = (const int *)arg;
    int core = a[0];
    int id   = a[1];
    pin_current_thread(core);

    for (uint32_t i = (uint32_t)id; i < num_chunks; i += NUM_WORKERS) {
        while (atomic_load_explicit(&chunks[i].state,
                                    memory_order_acquire) == 0)
            cpu_relax();

        fast_decompress(chunks[i].in_data, chunks[i].in_len,
                        chunks[i].out_data, chunks[i].out_len);

        atomic_store_explicit(&chunks[i].state, 2, memory_order_release);
    }
    return NULL;
}

void* write_worker(void* arg) {
    int fd = *(int*)arg;
    
    for (uint32_t i = 0; i < num_chunks; i++) {
        while (atomic_load_explicit(&chunks[i].state, memory_order_acquire) != 2) cpu_relax();

        size_t written = 0;
        while (written < chunks[i].out_len) {
            ssize_t res = write(fd, chunks[i].out_data + written, chunks[i].out_len - written);
            if (res > 0) written += res;
            else if (res < 0 && errno != EINTR) break;
        }
        free(chunks[i].in_data);
        free(chunks[i].out_data);
    }
    return NULL;
}

static int consumer(const char* shm_path, const char* file_path, int core_id) {
    int fd __attribute__((cleanup(cleanup_fd))) = -1;
    int shm_fd __attribute__((cleanup(cleanup_fd))) = -1;
    mmap_region_t shm_map __attribute__((cleanup(cleanup_mmap_region))) = {
        NULL, 
        0
    };
    global_ptr_guard_t chunks_guard __attribute__((cleanup(cleanup_global_ptr))) = {
        NULL
    };
    shm_unlink_guard_t unlink_guard __attribute__((cleanup(cleanup_shm_unlink))) = {
        NULL
    };


    char out_path[OUTPUT_FILE_PATH_SIZE] = {0};
    if (out_file(out_path, file_path) != 0) {
        return 1;
    }

    fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 1;
    }

    while ((shm_fd = shm_open(shm_path, O_RDWR, 0666)) == -1) {
        if (errno != ENOENT) {
            return 1;
        }
        usleep(100);
    }
    
    shm_map.ptr = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map.ptr == MAP_FAILED) {
        return 1;
    }
    shm_map.len = sizeof(shm_data);
    shm_data *shm = (shm_data *)shm_map.ptr;
    int slot = 0;
    unlink_guard.path = shm_path;

    shm_read(shm, &slot, &num_chunks, sizeof(num_chunks));
    
    chunks = (chunk*)calloc(num_chunks ? num_chunks : 1, sizeof(chunk));
    if (!chunks) {
        return 1;
    }
    chunks_guard.ref = (void **)&chunks;

    for (uint32_t i = 0; i < num_chunks; i++) {
        atomic_init(&chunks[i].state, 0);
    }

    pthread_t tids[NUM_WORKERS];
    int wargs[NUM_WORKERS][2];
    for (int i = 0; i < NUM_WORKERS; i++) {
        wargs[i][0] = get_worker_cpu_core(core_id, i);
        wargs[i][1] = i;
        pthread_create(&tids[i], NULL, decompress_worker, wargs[i]);
    }

    pthread_t wtid;
    pthread_create(&wtid, NULL, write_worker, &fd);

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t cs, us;
        shm_read(shm, &slot, &cs, sizeof(cs));
        shm_read(shm, &slot, &us, sizeof(us));

        chunks[i].in_len   = cs;
        chunks[i].out_len  = us;
        chunks[i].in_data  = (uint8_t*) malloc(cs);
        chunks[i].out_data = (uint8_t*) malloc(us);

        shm_read(shm, &slot, chunks[i].in_data, cs);
        atomic_store_explicit(&chunks[i].state, 1, memory_order_release);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }
    pthread_join(wtid, NULL);

    return 0;
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

static int get_worker_cpu_core(int main_core, int idx) {
    if (main_core == 0) return (idx == 0) ? 1 : 3;
    if (main_core == 2) return (idx == 0) ? 5 : 7;
    static const int t[4][2] = {
        {1,3},
        {0,2},
        {1,3},
        {0,2}
    };
    return t[main_core & 3][idx];
}
