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

typedef _Atomic(int32_t) ai32;

typedef struct {
    ai32 state;
    ai32 wr_bytes;
    char data[SHM_SIZE - sizeof(ai32) * 2];
} __attribute__((aligned(CACHE_LINE_SIZE))) shm_data;
static_assert(sizeof(shm_data) >= SHM_SIZE, "shm memory data struct is bigger than max size");

#define SHM_DATA_SIZE SHM_SIZE - sizeof(ai32) * 2

#endif
