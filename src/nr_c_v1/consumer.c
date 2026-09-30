#define _DEFAULT_SOURCE
#include "./../../include/cpu_utils.h"
#include "shm.h"

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
_Atomic int current_worker_idx = 0;

static void fast_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
static void shm_read(shm_data *shm, int *slot, void *dst, size_t size);
void* decompress_worker(void* arg);
void* write_worker(void* arg);
static int consumer(const char* shm_path, const char* file_path, int core_id);
static int out_file(char* output_file_buffer, const char* file_path);

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
    size_t ip = 0, op = 0;
    while (ip < in_len && op < out_len) {
        uint8_t token = in[ip++];
        size_t lit_len = token >> 4;
        if (lit_len == 15) {
            uint8_t l;
            do { l = in[ip++]; lit_len += l; } while (l == 255);
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
            do { l = in[ip++]; match_len += l; } while (l == 255);
        }

        for (size_t i = 0; i < match_len; i++) {
            out[op + i] = out[op - off + i];
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
    int core = *(int*)arg;
    pin_current_thread(core);
    free(arg);

    while (1) {
        int idx = atomic_fetch_add_explicit(&current_worker_idx, 1, memory_order_relaxed);
        if (idx >= (int)num_chunks) break;

        while (atomic_load_explicit(&chunks[idx].state, memory_order_acquire) == 0) cpu_relax();

        fast_decompress(chunks[idx].in_data, chunks[idx].in_len, chunks[idx].out_data, chunks[idx].out_len);
        atomic_store_explicit(&chunks[idx].state, 2, memory_order_release);
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
    char out_path[OUTPUT_FILE_PATH_SIZE] = {0};
    if (out_file(out_path, file_path) != 0) return 1;

    int fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;

    int shm_fd = -1;
    while ((shm_fd = shm_open(shm_path, O_RDWR, 0666)) == -1) {
        if (errno != ENOENT) {
            close(fd);
            return 1;
        }
        usleep(100);
    }

    shm_data *shm = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        close(fd);
        close(shm_fd);
        return 1;
    }
    int current_slot = 0;

    shm_read(shm, &current_slot, &num_chunks, sizeof(num_chunks));

    chunks = (chunk*)calloc(num_chunks, sizeof(chunk));
    for (uint32_t i = 0; i < num_chunks; i++) {
        atomic_init(&chunks[i].state, 0);
    }

    pthread_t tids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        int *core = (int*)malloc(sizeof(int));
        *core = (core_id + 2 * (i + 1)) % 8;
        pthread_create(&tids[i], NULL, decompress_worker, core);
    }

    pthread_t wtid;
    pthread_create(&wtid, NULL, write_worker, &fd);

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t cs, us;
        shm_read(shm, &current_slot, &cs, sizeof(cs));
        shm_read(shm, &current_slot, &us, sizeof(us));

        chunks[i].in_len = cs;
        chunks[i].out_len = us;
        chunks[i].in_data = (uint8_t*)malloc(cs);
        chunks[i].out_data = (uint8_t*)malloc(us);

        shm_read(shm, &current_slot, chunks[i].in_data, cs);
        atomic_store_explicit(&chunks[i].state, 1, memory_order_release);
    }

    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }
    pthread_join(wtid, NULL);

    free(chunks);
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);

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
