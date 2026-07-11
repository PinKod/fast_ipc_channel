#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>


static int producer(const char* shm_path, const char* file_path);




int main(int argc, char* argv[]) {
    if(argc != 3) return 1;
    
    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    if(producer(shm_path, file_path) != 0) return 1;

    return 0;
}




static int producer(const char* shm_path, const char* file_path) {
    int fd = open(file_path, O_RDONLY);
    if(fd == -1) return 1;

    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if(shm_fd == -1) {
        close(fd);
        return 1;
    }

    ftruncate(shm_fd, sizeof(shm_data));
    shm_data *shm = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(shm == MAP_FAILED) {
        close(fd);
        close(shm_fd);
        return 1;
    }

    atomic_init(&shm->state, STATE_EMPTY);
    ssize_t bytes;
    while (1) {
        while (atomic_load(&shm->state) == STATE_FULL) {
        }
        ssize_t bytes = read(fd, shm->data, SHM_DATA_SIZE);
        if (bytes <= 0) {
            break;
        }

        atomic_store(&shm->wr_bytes, bytes);
        atomic_store(&shm->state, STATE_FULL);
    }
    while (atomic_load(&shm->state) != STATE_EMPTY) {}

    atomic_store(&shm->state, STATE_EOF);
    close(fd);
    close(shm_fd);
    return 0;
}