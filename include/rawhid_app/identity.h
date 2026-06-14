#pragma once

#include <stdint.h>

/* Returns the stable per-device u64 hash used to identify this device to the host.
 * Returns 0 if neither hwinfo nor settings produced a seed (identity unavailable).
 * Thread-safe after SYS_INIT(APPLICATION) has completed. */
uint64_t rawhid_app_identity_get_uid_hash(void);

/* Returns the capability bitmask reflecting currently-enabled Kconfigs:
 *   bit 0 — CONFIG_RAWHID_APP_LAYER_CONTROL
 *   bit 1 — CONFIG_RAWHID_APP_TIME_SYNC
 *   bit 2 — CONFIG_RAWHID_APP_AI_USAGE
 *   bit 3 — THEME (always 0, no CONFIG yet)
 *   bit 4 — CONFIG_RAWHID_APP_BATTERY_REPORT
 *   bit 5 — CONFIG_RAWHID_APP_HOST_ACTION
 *   bit 6 — CONFIG_RAWHID_APP_KEY_STATS
 *   bit 7 — CONFIG_RAWHID_APP_LAYER_STATE_REPORT
 *   bit 8 — CONFIG_RAWHID_APP_KEY_PRESS */
uint32_t rawhid_app_identity_get_capabilities(void);
