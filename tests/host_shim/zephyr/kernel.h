#pragma once

#include <stddef.h>
#include <stdint.h>

struct k_work;

typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
    k_work_handler_t handler;
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
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CONTAINER_OF(ptr, type, field)                                                             \
    ((type *)(void *)((char *)(ptr) - offsetof(type, field)))

struct k_mutex {
    int unused;
};

static inline void k_mutex_lock(struct k_mutex *mutex, int timeout) {
    ARG_UNUSED(mutex);
    ARG_UNUSED(timeout);
}

static inline void k_mutex_unlock(struct k_mutex *mutex) {
    ARG_UNUSED(mutex);
}

static inline void k_work_init_delayable(struct k_work_delayable *work,
                                         k_work_handler_t handler) {
    work->work.handler = handler;
    work->last_delay_ms = 0;
}

static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}

static inline int k_work_reschedule(struct k_work_delayable *work, int64_t delay_ms) {
    work->last_delay_ms = delay_ms;
    return 0;
}
