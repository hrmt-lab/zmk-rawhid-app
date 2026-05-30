#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_RAWHID_APP_TIME_SYNC)
/* Format the current host-synced local time into `buf`. Returns false (buf
 * untouched) if no TIME_SYNC packet has been received yet. */
bool rawhid_app_time_sync_format(char *buf, size_t len);

/* True if the active format includes seconds (display should refresh faster). */
bool rawhid_app_time_sync_wants_seconds(void);
#else
static inline bool rawhid_app_time_sync_format(char *buf, size_t len) {
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    return false;
}

static inline bool rawhid_app_time_sync_wants_seconds(void) {
    return false;
}
#endif
