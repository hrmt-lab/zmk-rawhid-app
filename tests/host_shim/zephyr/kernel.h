#pragma once

#include <stdint.h>

struct k_mutex {
    int unused;
};

struct k_work {
    int unused;
};

struct k_work_delayable {
    struct k_work work;
    int64_t last_delay_ms;
};

#define K_FOREVER 0
#define K_SECONDS(seconds) ((int64_t)(seconds) * 1000)
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_WORK_DELAYABLE_DEFINE(name, handler) struct k_work_delayable name

#define ARG_UNUSED(value) ((void)(value))

static inline void k_mutex_lock(struct k_mutex *mutex, int timeout) {
    ARG_UNUSED(mutex);
    ARG_UNUSED(timeout);
}

static inline void k_mutex_unlock(struct k_mutex *mutex) {
    ARG_UNUSED(mutex);
}

static inline int k_work_reschedule(struct k_work_delayable *work, int64_t delay_ms) {
    work->last_delay_ms = delay_ms;
    return 0;
}
