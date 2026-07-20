#ifndef SHM_H
#define SHM_H

#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <stdalign.h>

typedef _Atomic(int32_t) ai32;

#define STATE_EMPTY 0
#define STATE_FULL  1
#define STATE_EOF   2

#define SHM_SIZE      256
#define SHM_SLOT_SIZE 128
#define SHM_DATA_SIZE (SHM_SLOT_SIZE - (int)sizeof(ai32) * 2)

typedef struct {
    ai32 state;
    ai32 wr_bytes;
    char data[SHM_DATA_SIZE];
} __attribute__((aligned(SHM_SLOT_SIZE))) shm_slot;
static_assert(sizeof(shm_slot) == SHM_SLOT_SIZE, "shm slot must be 128 bytes");

typedef struct {
    shm_slot slots[2];
} __attribute__((aligned(SHM_SIZE))) shm_data;
static_assert(sizeof(shm_data) == SHM_SIZE, "SHM data struct must be 256 bytes");

typedef struct {
    uint8_t *in_data;
    size_t in_len;
    uint8_t *out_data;
    size_t out_len;
    _Atomic int state;
    char _pad[28];
} __attribute__((aligned(64))) chunk;
static_assert(sizeof(chunk) == 64, "chunk must be 64 bytes");


typedef struct {
    uint32_t cs;
    uint32_t us;
} chunk_hdr_t;

#endif //SHM_H