#define _DEFAULT_SOURCE
#include "./../../include/cpu_utils.h"
#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>


#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#elif defined(__aarch64__)
    #define cpu_relax() __asm__ volatile("yield\n": : :"memory")
#else
    #define cpu_relax() do {} while(0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PREFETCH_AHEAD_CHUNKS 4

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
    unfair_sched();
    int fd = open(file_path, O_RDONLY);
    if(fd == -1) return 1;

    struct stat st;
    if(unlikely(stat(file_path, &st)) == -1) {
        close(fd);
        return 1;
    }

    size_t file_size = st.st_size;
    char* file_mapping = NULL;
    if (file_size > 0) {
        file_mapping = (char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (file_mapping == MAP_FAILED) {
            close(fd);
            return 1;
        }
        madvise(file_mapping, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);
    }

    int shm_fd = shm_open(shm_path, O_CREAT | O_RDWR, 0666);
    if(unlikely(shm_fd == -1)) {
        close(fd);
        return 1;
    }

    ftruncate(shm_fd, sizeof(shm_data));
    shm_data *shm = (shm_data *)mmap(NULL, sizeof(shm_data), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(unlikely(shm == MAP_FAILED)) {
        close(fd);
        close(shm_fd);
        return 1;
    }

    atomic_init(&shm->ctrl, SHM_PACK(STATE_EMPTY, 0));
    size_t offset = 0;

    uint64_t spin_count = 0;
    uint64_t wait_events = 0;

    while (offset < file_size) {
        int spins_this_round = 0;
        while (SHM_UNPACK_STATE(atomic_load_explicit(&shm->ctrl, memory_order_acquire)) == STATE_FULL) {
            cpu_relax();
            spins_this_round++;
        }

        size_t chunk_size = SHM_DATA_SIZE;
        if(offset + chunk_size > file_size) {
            chunk_size = file_size - offset;
        }

        // v10: adds software prefetch ahead of the copy loop (vs v9, which had none)
        size_t prefetch_offset = offset + PREFETCH_AHEAD_CHUNKS * SHM_DATA_SIZE;
        if (prefetch_offset < file_size) {
            __builtin_prefetch(file_mapping + prefetch_offset, 0, 3);
        }

        memcpy(shm->data, file_mapping + offset, chunk_size);
        atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_FULL, chunk_size), memory_order_release);
        offset += chunk_size;

        if (spins_this_round > 0) {
            spin_count += spins_this_round;
            wait_events++;
        }
    }
    while (SHM_UNPACK_STATE(atomic_load_explicit(&shm->ctrl, memory_order_acquire)) != STATE_EMPTY) {
        cpu_relax();
    }

    printf("Producer total spins: %lu (across %lu waits)\n", spin_count, wait_events);

    atomic_store_explicit(&shm->ctrl, SHM_PACK(STATE_EOF, 0), memory_order_release);
    if(file_mapping) {
        munmap(file_mapping, file_size);
    }
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    return 0;
}
