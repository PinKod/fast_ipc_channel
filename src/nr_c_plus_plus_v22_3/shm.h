#ifndef SHM_H
#define SHM_H

#include <atomic>
#include <cstdint>
#include <cassert>

#define STATE_EOF -1
#define STATE_EMPTY 0
#define STATE_FULL 1
#define STATE_BLOCK_HEADER 2

#define SHM_SIZE 256
#define CACHE_LINE_SIZE 64

typedef std::atomic<int64_t> ai64;

struct alignas(CACHE_LINE_SIZE) shm_data {
    ai64 ctrl;
    char data[SHM_SIZE - sizeof(ai64)];
};
static_assert(sizeof(shm_data) >= SHM_SIZE, "shm memory data struct is bigger than max size");

#define SHM_DATA_SIZE (SHM_SIZE - sizeof(ai64))

#define SHM_PACK(state, size)   ((((int64_t)(uint32_t)(size)) << 32) | (uint32_t)(int32_t)(state))
#define SHM_UNPACK_STATE(ctrl)  ((int32_t)((ctrl) & 0xFFFFFFFFLL))
#define SHM_UNPACK_SIZE(ctrl)   ((int32_t)((int64_t)(ctrl) >> 32))

#define SHM_LANES 4
struct shm_ring {
    shm_data lanes[SHM_LANES];
};

#define LZ4_BLOCK_SIZE (128 * 1024)
struct lz4_block_header {
    uint32_t block_id;
    uint32_t comp_size;
    uint32_t uncomp_size;
};
static_assert(sizeof(lz4_block_header) <= SHM_DATA_SIZE, "lz4_block_header must fit in one shm chunk");

#endif
