#ifndef CPU_UTILS_H
#define CPU_UTILS_H

#define _GNU_SOURCE

#include <sched.h>
#include <unistd.h>
#include <stdio.h>

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

static inline void unfair_sched() {
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("Notice: Could not set SCHED_FIFO (run runner with sudo for max perf)");
    }
}

#endif //CPU_UTILS_H