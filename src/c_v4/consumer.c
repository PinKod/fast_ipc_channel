#define _DEFAULT_SOURCE
#include "./../../include/cpu_utils.h"
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



#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)


static int decompress_block(const uint8_t* in, uint32_t* out);
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




static int consumer(const char* shm_path, const char* file_path, int core_id) {
    int fd __attribute__((cleanup(cleanup_fd))) = -1;
    int shm_fd __attribute__((cleanup(cleanup_fd))) = -1;
    mmap_region_t shm_map_reg __attribute__((cleanup(cleanup_mmap_region))) = {NULL, 0};
    shm_unlink_guard_t unlink_guard __attribute__((cleanup(cleanup_shm_unlink))) = {NULL};

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

    shm_map_reg.ptr = mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map_reg.ptr == MAP_FAILED) {
        return 1;
    }
    shm_map_reg.len = sizeof(shm_data);

    shm_data *shm = (shm_data *)shm_map_reg.ptr;
    unlink_guard.path = shm_path;

    uint32_t total_floats;
    shm_recv(shm, &total_floats, 4);

    uint32_t floats_read = 0;
    uint8_t out_buf[320];

    while (floats_read < total_floats) {
        uint32_t msg_len;
        shm_recv(shm, &msg_len, 4);
        if (msg_len == 0) {
            break;
        }

        uint8_t buf[324];
        shm_recv(shm, buf, msg_len);

        uint32_t floats[64];
        int count = decompress_block(buf, floats);

        for (int i = 0; i < count; i++) {
            memcpy(out_buf + i * 5, &floats[i], 4);
            out_buf[i * 5 + 4] = '\n';
        }

        size_t written = 0;
        size_t to_write = count * 5;
        while (written < to_write) {
            ssize_t res = write(fd, out_buf + written, to_write - written);
            if (res > 0) written += res;
            else if (res < 0 && errno != EINTR) break;
        }

        floats_read += count;
    }

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

static int decompress_block(const uint8_t* in, uint32_t* out) {
    uint8_t hdr = in[0];
    int count = hdr & 0x7F;
    int is_raw = hdr & 0x80;

    if (is_raw) {
        memcpy(out, in + 1, count * 4);
        return count;
    }

    size_t ip = 1;
    uint32_t prev = 0;
    for (int i = 0; i < count; i++) {
        uint8_t tag = in[ip++];
        uint32_t diff = 0;
        if (tag == 0) {
            diff = 0;
        } else if (tag == 1) {
            diff = in[ip++];
        } else if (tag == 2) {
            diff = in[ip] | (in[ip+1] << 8); ip += 2;
        } else if (tag == 3) {
            diff = in[ip] | (in[ip+1] << 8) | (in[ip+2] << 16); ip += 3;
        } else {
            diff = *(uint32_t*)(in + ip); ip += 4;
        }
        uint32_t val = prev ^ diff;
        out[i] = val;
        prev = val;
    }
    return count;
}
