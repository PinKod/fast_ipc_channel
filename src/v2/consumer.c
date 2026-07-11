
#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#define OUTPUT_FILE_PATH_SIZE 512
#define MB 1024 * 1024
#define LOCAL_BUF_SIZE (MB)

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

    int fd = open(output_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd == -1) return 1;

    int shm_fd = -1;
    while ((shm_fd = shm_open(shm_path, O_RDWR, 0666)) == -1) {
        if (errno != ENOENT) {
            close(fd);
            return 1;
        }
        usleep(100);
    }
    shm_data *shm = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(shm == MAP_FAILED) {
        close(fd);
        close(shm_fd);
        return 1;
    }

    char *buffer = (char*)malloc(LOCAL_BUF_SIZE);
    if(buffer == NULL) {
        munmap(shm, sizeof(shm_data));
        close(fd);
        close(shm_fd);
        return 1;
    }

    size_t buffer_offset = 0;
    while (1) {
        while (atomic_load(&shm->state) == STATE_EMPTY) {}
        
        int state = atomic_load(&shm->state);
        if(state == STATE_EOF) {
            break;
        }

        int size = atomic_load(&shm->wr_bytes);
        memcpy(buffer + buffer_offset, shm->data, size);
        buffer_offset += size;
        if(buffer_offset + SHM_DATA_SIZE > LOCAL_BUF_SIZE) {
            ssize_t writen_bytes = 0;
            while(writen_bytes < buffer_offset) {
                ssize_t num = write(fd, buffer + writen_bytes, buffer_offset - writen_bytes);
                if(num <= 0) {
                    if(errno == EINTR) continue;
                    break;
                }
                writen_bytes += num;
            }
            buffer_offset = 0;
        }
        atomic_store(&shm->state, STATE_EMPTY);
    }
    
    if(buffer_offset > 0) {
        ssize_t writen_bytes = 0;
        while(writen_bytes < buffer_offset) {
            ssize_t num = write(fd, buffer + writen_bytes, buffer_offset - writen_bytes);
            if(num <= 0) {
                if(errno == EINTR) continue;
                break;
            }
            writen_bytes += num;
        }
    }

    atomic_store(&shm->state, STATE_EOF);
    free(buffer);
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);
    return 0;
}