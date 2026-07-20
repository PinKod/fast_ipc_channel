#include "./../include/cpu_utils.h"
#include "shm.h"

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdalign.h>

#define OUTPUT_FILE_PATH_SIZE 512
#define MB (1024 * 1024)
#define LOCAL_BUF_SIZE (MB)
#define BUFFER_NUM 2
#define CACHE_LINE_SIZE 64

#define BUF_STATE_EMPTY 0
#define BUF_STATE_FULL 1
#define BUF_STATE_DONE 2

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n": : :"memory")
#elif defined(__aarch64__)
    #define cpu_relax() __asm__ volatile("yield\n": : :"memory")
#else
    #define cpu_relax() do {} while(0)
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

typedef struct {
    char* data;
    size_t length;
    _Atomic int state;
} __attribute__((aligned(CACHE_LINE_SIZE))) buffer_ctx;

typedef struct {
    buffer_ctx buf[BUFFER_NUM];
    int fd;
} thread_ctx;



static int consumer(const char* shm_path, const char* file_path);
static int out_file(char* output_file_buffer, const char* file_path);
void* write_thread(void* arg);




int main(int argc, char* argv[]) {
    if(argc != 4) return 1;

    const char* shm_path = argv[1];
    const char* file_path = argv[2];
    int core_id = atoi(argv[3]);
    if(pin_current_thread(core_id) != 0) return 1;
    if(consumer(shm_path, file_path) != 0) return 1;

    return 0;
}




void* write_thread(void* arg) {
    thread_ctx* ctx = (thread_ctx*)arg;
    pin_current_thread(get_current_cpu_core() + 1);
    int current_buf = 0;
    while(1) {
        int state;
        while((state = atomic_load_explicit(&ctx->buf[current_buf].state, memory_order_acquire)) == BUF_STATE_EMPTY) {
            cpu_relax();
        }
        if(unlikely(state == BUF_STATE_DONE)){
            break;
        }

        const size_t size = ctx->buf[current_buf].length;
        ssize_t written_bytes = 0;
        while (written_bytes < size) {
            ssize_t num = write(ctx->fd, ctx->buf[current_buf].data + written_bytes, size - written_bytes);
            if (unlikely(num <= 0)) {
                if (likely(errno == EINTR)) continue;
                break;
            }
            written_bytes += num;
        }

        atomic_store_explicit(&ctx->buf[current_buf].state, BUF_STATE_EMPTY, memory_order_release);
        current_buf = 1 - current_buf; 
    }

    return NULL;
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

    thread_ctx ctx;
    ctx.fd = fd;
    for (int i = 0; i < BUFFER_NUM; i++) {
        posix_memalign((void**)&ctx.buf[i].data, CACHE_LINE_SIZE, LOCAL_BUF_SIZE);
        atomic_init(&ctx.buf[i].state, BUF_STATE_EMPTY);
    }
    pthread_t writer_tid;
    pthread_create(&writer_tid, NULL, write_thread, &ctx);


    size_t buffer_offset = 0;
    int state;
    int current_buf = 0;

    uint64_t spin_count = 0;
    uint64_t wait_events = 0;

    while (1) {
        int spins_this_round = 0;
        while ((state = atomic_load_explicit(&shm->state, memory_order_acquire)) == STATE_EMPTY) {
            cpu_relax();
            spins_this_round++;
        }

        if(unlikely(state == STATE_EOF)) {
            break;
        }

        int size = atomic_load_explicit(&shm->wr_bytes, memory_order_relaxed);
        memcpy(ctx.buf[current_buf].data + buffer_offset, shm->data, size);
        buffer_offset += size;
        atomic_store_explicit(&shm->state, STATE_EMPTY, memory_order_release);

        if(buffer_offset + SHM_DATA_SIZE > LOCAL_BUF_SIZE) {
            ctx.buf[current_buf].length = buffer_offset;
            atomic_store_explicit(&ctx.buf[current_buf].state, BUF_STATE_FULL, memory_order_release);
            
            current_buf = 1 - current_buf;
            buffer_offset = 0;

            while (atomic_load_explicit(&ctx.buf[current_buf].state, memory_order_acquire) != BUF_STATE_EMPTY) {
                cpu_relax();
            }
        }

        if (spins_this_round > 0) {
            spin_count += spins_this_round;
            wait_events++;
        }
    }

    if(buffer_offset > 0) {
        ctx.buf[current_buf].length = buffer_offset;
        atomic_store_explicit(&ctx.buf[current_buf].state, BUF_STATE_FULL, memory_order_release);
        current_buf = 1 - current_buf;
        while (atomic_load_explicit(&ctx.buf[current_buf].state, memory_order_acquire) != BUF_STATE_EMPTY) {
            cpu_relax();
        }
    }

    printf("Consumer total spins: %lu (across %lu waits)\n", spin_count, wait_events);

    atomic_store_explicit(&ctx.buf[current_buf].state, BUF_STATE_DONE, memory_order_release);
    pthread_join(writer_tid, NULL);
    for (int i = 0; i < BUFFER_NUM; i++) {
        free(ctx.buf[i].data);
    }

    atomic_store_explicit(&shm->state, STATE_EOF, memory_order_release);
    munmap(shm, sizeof(shm_data));
    close(fd);
    close(shm_fd);
    shm_unlink(shm_path);
    return 0;
}