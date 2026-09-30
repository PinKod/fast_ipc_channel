#ifndef CLEANUP_H
#define CLEANUP_H

#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/stat.h>



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


#endif
