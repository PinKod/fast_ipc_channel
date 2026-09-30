#ifndef SHM_H
#define SHM_H

#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <stdalign.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
    #define cpu_relax() __asm__ volatile("pause\n" ::: "memory")
#else
    #error
#endif

typedef _Atomic(int32_t) ai32;


#define SHM_SIZE 256
typedef struct {
    _Atomic uint32_t head;
    char pad0[60];

    _Atomic uint32_t tail;
    char pad1[60];

    char buf[128];
} __attribute__((aligned(SHM_SIZE))) shm_data;
static_assert(sizeof(shm_data) == SHM_SIZE, "shm size must be 256 bytes");

static inline void shm_send(shm_data* shm, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t written = 0;
    while (written < len) {
        uint32_t h = atomic_load_explicit(&shm->head, memory_order_relaxed);
        uint32_t t = atomic_load_explicit(&shm->tail, memory_order_acquire);
        uint32_t space = 128 - (h - t);
        if (space == 0) {
            cpu_relax();
            continue;
        }

        size_t to_write = len - written;
        if (to_write > space) to_write = space;

        size_t start = h & 127;
        size_t end = (h + to_write) & 127;
        if (start < end) {
            memcpy(&shm->buf[start], p + written, to_write);
        } else {
            size_t first = 128 - start;
            memcpy(&shm->buf[start], p + written, first);
            memcpy(&shm->buf[0], p + written + first, to_write - first);
        }
        h += to_write;
        written += to_write;
        atomic_store_explicit(&shm->head, h, memory_order_release);
    }
}

static inline void shm_recv(shm_data* shm, void* data, size_t len) {
    uint8_t* p = (uint8_t*)data;
    size_t read_bytes = 0;
    while (read_bytes < len) {
        uint32_t t = atomic_load_explicit(&shm->tail, memory_order_relaxed);
        uint32_t h = atomic_load_explicit(&shm->head, memory_order_acquire);
        uint32_t avail = h - t;
        if (avail == 0) {
            cpu_relax();
            continue;
        }

        size_t to_read = len - read_bytes;
        if (to_read > avail) to_read = avail;

        size_t start = t & 127;
        size_t end = (t + to_read) & 127;
        if (start < end) {
            memcpy(p + read_bytes, &shm->buf[start], to_read);
        } else {
            size_t first = 128 - start;
            memcpy(p + read_bytes, &shm->buf[start], first);
            memcpy(p + read_bytes + first, &shm->buf[0], to_read - first);
        }
        t += to_read;
        read_bytes += to_read;
        atomic_store_explicit(&shm->tail, t, memory_order_release);
    }
}


typedef struct {
    uint8_t data[324];
    uint32_t len;
    _Atomic int state;
    char _pad[52];
} __attribute__((aligned(64))) block_t;


#endif
