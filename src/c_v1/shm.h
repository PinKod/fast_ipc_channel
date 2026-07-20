#ifndef SHM_H
#define SHM_H

#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <stdalign.h>

#define STATE_EMPTY 0
#define STATE_FULL  1
#define STATE_EOF   2

#define SHM_SIZE 256
#define SHM_SLOT_SIZE 128

#define SHM_DATA_SIZE (SHM_SLOT_SIZE - sizeof(ai32) * 2)

typedef _Atomic(int32_t) ai32;

typedef struct {
    ai32 state;
    ai32 wr_bytes;  
    char data[SHM_DATA_SIZE];
} __attribute__((aligned(128))) shm_slot;

typedef struct {
    shm_slot slots[2];
} shm_data;

static_assert(sizeof(shm_data) == SHM_SIZE, "SHM data struct must be exactly 256 bytes");

typedef struct {
    uint8_t *in_data;
    size_t in_len;
    uint8_t *out_data;
    size_t out_len;
    _Atomic int state;
} chunk;


#endif //SHM_H