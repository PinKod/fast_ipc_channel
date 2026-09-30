#ifndef SHM_H
#define SHM_H

#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <stdalign.h>

#define STATE_EOF -1
#define STATE_EMPTY 0
#define STATE_FULL 1

#define SHM_SIZE 256
#define CACHE_LINE_SIZE 64

typedef _Atomic(int64_t) ai64;

typedef struct {
    ai64 ctrl; // v9: replaces v8's separate state+wr_bytes atomics with one packed word
    char data[SHM_SIZE - sizeof(ai64)];
} __attribute__((aligned(CACHE_LINE_SIZE))) shm_data;
static_assert(sizeof(shm_data) >= SHM_SIZE, "shm memory data struct is bigger than max size");

#define SHM_DATA_SIZE (SHM_SIZE - sizeof(ai64))

#define SHM_PACK(state, size)   ((((int64_t)(uint32_t)(size)) << 32) | (uint32_t)(int32_t)(state))
#define SHM_UNPACK_STATE(ctrl)  ((int32_t)((ctrl) & 0xFFFFFFFFLL))
#define SHM_UNPACK_SIZE(ctrl)   ((int32_t)((int64_t)(ctrl) >> 32))

#endif
