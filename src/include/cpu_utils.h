#ifndef CPU_UTILS_H
#define CPU_UTILS_H

#define _GNU_SOURCE

#include <sched.h>
#include <unistd.h>

static inline int get_current_cpu_core() {
    const int core = sched_getcpu();
    return (core < 0) ? 0 : core;
}

static inline int pin_current_thread(int cpu_core) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_core, &cpu_set);
    return sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set);
}

#endif //CPU_UTILS_H