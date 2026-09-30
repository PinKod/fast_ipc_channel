#ifndef SHM_H
#define SHM_H

#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <stdalign.h>

#define STATE_EOF -1
#define STATE_EMPTY 0
#define STATE_FULL 1
#define STATE_BLOCK_HEADER 2

#define SHM_SIZE 256
#define CACHE_LINE_SIZE 64

typedef _Atomic(int64_t) ai64;

typedef struct {
    ai64 ctrl;
    char data[SHM_SIZE - sizeof(ai64)];
} __attribute__((aligned(CACHE_LINE_SIZE))) shm_data;
static_assert(sizeof(shm_data) >= SHM_SIZE, "shm memory data struct is bigger than max size");

#define SHM_DATA_SIZE (SHM_SIZE - sizeof(ai64))

#define SHM_PACK(state, size)   ((((int64_t)(uint32_t)(size)) << 32) | (uint32_t)(int32_t)(state))
#define SHM_UNPACK_STATE(ctrl)  ((int32_t)((ctrl) & 0xFFFFFFFFLL))
#define SHM_UNPACK_SIZE(ctrl)   ((int32_t)((int64_t)(ctrl) >> 32))

#define SHM_LANES 4
typedef struct {
    shm_data lanes[SHM_LANES];
} shm_ring;

#define RECORD_SIZE 5
#define RECORDS_PER_BLOCK 200000
#define BLOCK_RECORD_BYTES (RECORDS_PER_BLOCK * RECORD_SIZE)
#define BLOCK_XOR_BYTES    (RECORDS_PER_BLOCK * 4)

typedef struct {
    uint32_t block_id;
    uint32_t comp_size;
    uint32_t record_count; // c_v23: repurposes c_v22's uncomp_size field; 0 means a raw uncompressed tail block
} lz4_block_header;
static_assert(sizeof(lz4_block_header) <= SHM_DATA_SIZE, "lz4_block_header must fit in one shm chunk");

#endif
