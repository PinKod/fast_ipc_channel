#ifndef CLEANUP_H
#define CLEANUP_H

#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdint.h>



static inline void cleanup_fd(int *fd)
    __attribute__((always_inline));
static inline void cleanup_fd(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}


typedef struct { void *ptr; size_t len; } mmap_region_t;
static inline void cleanup_mmap_region(mmap_region_t *r)
    __attribute__((always_inline));
static inline void cleanup_mmap_region(mmap_region_t *r) {
    if (r->ptr && r->ptr != MAP_FAILED) {
        munmap(r->ptr, r->len);
        r->ptr = NULL;
        r->len = 0;
    }
}




typedef struct {
    uint8_t *base;
    size_t   capacity;
    size_t   offset;
} arena_t;

static inline void cleanup_arena(arena_t *a) __attribute__((always_inline));
static inline void cleanup_arena(arena_t *a) {
    if (a->base && a->base != (uint8_t *)MAP_FAILED) {
        munmap(a->base, a->capacity);
        a->base = NULL;
    }
}

static inline int arena_init(arena_t *a, size_t size) {
    a->capacity = size;
    a->offset   = 0;
    a->base     = (uint8_t*)mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    return (a->base == MAP_FAILED) ? -1 : 0;
}

static inline void *arena_alloc(arena_t *a, size_t size)
    __attribute__((alloc_size(2)));
static inline void *arena_alloc(arena_t *a, size_t size) {
    size_t aligned = (a->offset + 63u) & ~(size_t)63u;
    if (aligned + size > a->capacity) return NULL;
    void  *ptr     = a->base + aligned;
    a->offset      = aligned + size;
    return ptr;
}





static inline void cleanup_free(void **p)
    __attribute__((always_inline));
static inline void cleanup_free(void **p) {
    if (*p) { free(*p); *p = NULL; }
}


typedef struct { void **ref; } global_ptr_guard_t;
static inline void cleanup_global_ptr(global_ptr_guard_t *g)
    __attribute__((always_inline));
static inline void cleanup_global_ptr(global_ptr_guard_t *g) {
    if (g->ref && *g->ref) { free(*g->ref); *g->ref = NULL; }
}


typedef struct { const char *path; } shm_unlink_guard_t;
static inline void cleanup_shm_unlink(shm_unlink_guard_t *g)
    __attribute__((always_inline));
static inline void cleanup_shm_unlink(shm_unlink_guard_t *g) {
    if (g->path) shm_unlink(g->path);
}


#endif //CLEANUP_H