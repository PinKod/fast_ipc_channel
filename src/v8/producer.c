#include "./../../include/cpu_utils.h"
#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
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

#define LOCAL_BUF_SIZE (1024 * 1024)
#define BUFFER_NUM 2
#define CACHE_LINE_SIZE 64

#define BUF_STATE_EMPTY 0
#define BUF_STATE_FULL 1
#define BUF_STATE_DONE 2


typedef struct {
    char* data;
    size_t length;
    _Atomic int state;
} __attribute__((aligned(CACHE_LINE_SIZE))) buffer_ctx;

typedef struct {
    buffer_ctx buf[BUFFER_NUM];
    int fd;
} producer_ctx;



static int producer(const char* shm_path, const char* file_path);
void *read_thread(void* arg);



int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(producer(shm_path, file_path) != 0) return 1;

    return 0;
}



void *read_thread(void* arg) {
    producer_ctx* ctx = (producer_ctx*)arg;
    pin_current_thread(get_current_cpu_core() + 1);

    int current_buf = 0;
    while (1) {
        while (atomic_load_explicit(&ctx->buf[current_buf].state, memory_order_acquire) != BUF_STATE_EMPTY) {
            cpu_relax();
        }

        ssize_t bytes_read = read(ctx->fd, ctx->buf[current_buf].data, LOCAL_BUF_SIZE);

        if (unlikely(bytes_read <= 0)) {
            ctx->buf[current_buf].length = 0;
            atomic_store_explicit(&ctx->buf[current_buf].state, BUF_STATE_DONE, memory_order_release);
            break;
        }

        ctx->buf[current_buf].length = bytes_read;
        atomic_store_explicit(&ctx->buf[current_buf].state, BUF_STATE_FULL, memory_order_release);
        current_buf = 1 - current_buf;
    }
    return NULL;
}

static int producer(const char* shm_path, const char* file_path) {
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
        file_mapping = (char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (file_mapping == MAP_FAILED) {
            close(fd);
            return 1;
        }
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

    producer_ctx ctx;
    ctx.fd = fd;
    for (int i = 0; i < BUFFER_NUM; i++) {
        ctx.buf[i].data = (char *)malloc(LOCAL_BUF_SIZE);
        atomic_init(&ctx.buf[i].state, BUF_STATE_EMPTY);
    }
    pthread_t writer_tid;
    pthread_create(&writer_tid, NULL, read_thread, &ctx);

    atomic_init(&shm->state, STATE_EMPTY);
    size_t offset = 0;

    uint64_t spin_count = 0;
    uint64_t wait_events = 0;
    int current_buf = 0;
    while (offset < file_size) {
        int buf_state;
        int spins_this_round = 0;


        while ((buf_state = atomic_load_explicit(&ctx.buf[current_buf].state, memory_order_acquire)) == BUF_STATE_EMPTY) {
            cpu_relax();
            spins_this_round++;
        }

        if (unlikely(buf_state == BUF_STATE_DONE)) {
            break;
        }

        size_t offset = 0;
        size_t total_length = ctx.buf[current_buf].length;

        while (offset < total_length) {
            int spins_this_round = 0;
            while (atomic_load_explicit(&shm->state, memory_order_acquire) != STATE_EMPTY) {
                cpu_relax();
                spins_this_round++;
            }
            if (spins_this_round > 0) {
                spin_count += spins_this_round;
                wait_events++;
            }

            size_t chunk_size = total_length - offset;
            if (chunk_size > SHM_DATA_SIZE) {
                chunk_size = SHM_DATA_SIZE;
            }

            memcpy(shm->data, ctx.buf[current_buf].data + offset, chunk_size);
            atomic_store_explicit(&shm->wr_bytes, chunk_size, memory_order_relaxed);
            atomic_store_explicit(&shm->state, STATE_FULL, memory_order_release);
            offset += chunk_size;
        }

        atomic_store_explicit(&ctx.buf[current_buf].state, BUF_STATE_EMPTY, memory_order_release);
        current_buf = 1 - current_buf;
    }

    while (atomic_load_explicit(&shm->state, memory_order_acquire) != STATE_EMPTY) {
        cpu_relax();
    }

    pthread_join(writer_tid, NULL);
    atomic_store_explicit(&shm->state, STATE_EOF, memory_order_release);
    for (int i = 0; i < BUFFER_NUM; i++) {
        free(ctx.buf[i].data);
    }
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);

    printf("Producer total spins: %lu (across %lu waits)\n", spin_count, wait_events);
    return 0;
}
