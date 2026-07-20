#include "./../include/cpu_utils.h"
#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>


static int producer(const char* shm_path, const char* file_path);




int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(producer(shm_path, file_path) != 0) return 1;

    return 0;
}




static int producer(const char* shm_path, const char* file_path) {
    int fd = open(file_path, O_RDONLY);
    if(fd == -1) return 1;

    struct stat st;
    if(stat(file_path, &st) == -1) {
        close(fd);
        return 1;
    }

    size_t file_size = st.st_size;
    char* file_mapping = NULL;
    if (file_size > 0) {
        file_mapping = (char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (file_mapping == MAP_FAILED) {
            close(fd);
            return 1;
        }
    }

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
    size_t offset = 0;
    while (offset < file_size) {
        while (atomic_load(&shm->state) == STATE_FULL) {}

        size_t chunk_size = SHM_DATA_SIZE;
        if(offset + chunk_size > file_size) {
            chunk_size = file_size - offset;
        }
        memcpy(shm->data, file_mapping + offset, chunk_size);
        atomic_store(&shm->wr_bytes, chunk_size);
        atomic_store(&shm->state, STATE_FULL);
        offset += chunk_size;
    }
    while (atomic_load(&shm->state) != STATE_EMPTY) {}

    atomic_store(&shm->state, STATE_EOF);
    if(file_mapping) {
        munmap(file_mapping, file_size);
    }
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    return 0;
}