#ifndef UMWAIT_H
#define UMWAIT_H

#include <x86gprintrin.h>
#include <immintrin.h>
#include <stdint.h>

static inline void wait_for_change_umwait(volatile void* addr, uint64_t timeout_cycles) {
    _umonitor((void*)addr);
    uint64_t deadline = __rdtsc() + timeout_cycles;
    _umwait(0, deadline);
}

#endif
