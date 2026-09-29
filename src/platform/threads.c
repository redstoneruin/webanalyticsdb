#include "../core/engine.h"
#include <errno.h>
#include <time.h>

int wadb_condition_init(pthread_cond_t *condition) {
    pthread_condattr_t attributes;
    int rc = pthread_condattr_init(&attributes);
    if (rc) return rc;
#ifndef __APPLE__
    rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
    if (!rc) rc = pthread_cond_init(condition, &attributes);
    pthread_condattr_destroy(&attributes);
    return rc;
}

int wadb_condition_wait_until(pthread_cond_t *condition, pthread_mutex_t *mutex, uint64_t deadline) {
#ifdef __APPLE__
    uint64_t now = wadb_monotonic_ns();
    if (now >= deadline) return ETIMEDOUT;
    uint64_t left = deadline - now;
    struct timespec relative = {.tv_sec = (time_t)(left / 1000000000), .tv_nsec = (long)(left % 1000000000)};
    return pthread_cond_timedwait_relative_np(condition, mutex, &relative);
#else
    struct timespec absolute = {.tv_sec = (time_t)(deadline / 1000000000), .tv_nsec = (long)(deadline % 1000000000)};
    return pthread_cond_timedwait(condition, mutex, &absolute);
#endif
}
