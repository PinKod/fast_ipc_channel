#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define OUTPUT_FILE_PATH_SIZE 512
#define MB (1024 * 1024)
#define LOCAL_BUF_SIZE (MB)

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#elif defined(__aarch64__)
    #define cpu_relax() __asm__ volatile("yield\n": : :"memory")
#else
    #define cpu_relax() do {} while(0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)



static int consumer(const char* shm_path, const char* file_path);
static int out_file(char* output_file_buffer, const char* file_path);



int main(int argc, char* argv[]) {
    if(argc != 3) return 1;
    
    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    if(consumer(shm_path, file_path) != 0) return 1;

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

static int consumer(const char* shm_path, const char* file_path) {
    char output_file_path[OUTPUT_FILE_PATH_SIZE] = {0};
    if(out_file(output_file_path, file_path) != 0) return 1;

    int fd = open(output_file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if(unlikely(fd == -1)) return 1;

    int shm_fd = -1;
    while ((shm_fd = shm_open(shm_path, O_RDWR, 0666)) == -1) {
        if (unlikely(errno != ENOENT)) {
            close(fd);
            return 1;
        }
        usleep(100);
    }
    shm_data *shm = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(unlikely(shm == MAP_FAILED)) {
        close(fd);
        close(shm_fd);
        return 1;
    }

    int state;
    while ((state = atomic_load_explicit(&shm->state, memory_order_acquire)) != STATE_INIT) {
        if (unlikely(state == STATE_EOF)) {
            munmap(shm, sizeof(shm_data));
            close(fd);
            close(shm_fd);
            return 1; 
        }
        cpu_relax();
    }

    size_t file_size = *(size_t*)(shm->data);
    if (ftruncate(fd, file_size) == -1) {
        munmap(shm, sizeof(shm_data));
        close(fd);
        close(shm_fd);
        return 1;
    }

    char* out_mapping = NULL;
    if (file_size > 0) {
        out_mapping = (char *)mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (unlikely(out_mapping == MAP_FAILED)) {
            munmap(shm, sizeof(shm_data));
            close(fd);
            close(shm_fd);
            return 1;
        }
    }
    
    atomic_store_explicit(&shm->state, STATE_EMPTY, memory_order_release);
    size_t buffer_offset = 0;
    while (1) {
        while ((state = atomic_load_explicit(&shm->state, memory_order_acquire)) == STATE_EMPTY) {
            cpu_relax();
        }

        if(unlikely(state == STATE_EOF)) {
            break;
        }

        int size = atomic_load_explicit(&shm->wr_bytes, memory_order_relaxed);
        memcpy(out_mapping + buffer_offset, shm->data, size);
        buffer_offset += size;
        atomic_store_explicit(&shm->state, STATE_EMPTY, memory_order_release);
    }

    atomic_store_explicit(&shm->state, STATE_EOF, memory_order_release);
    if (out_mapping) {
        munmap(out_mapping, file_size);
    }
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);
    return 0;
}