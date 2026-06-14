#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <rawhid_app/identity.h>

/* FNV-1a 64-bit constants. */
#define FNV1A_64_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV1A_64_PRIME        UINT64_C(1099511628211)

static uint64_t uid_hash; /* 0 until resolved; static storage initialises to 0 */

static uint64_t compute_uid_hash(const uint8_t *seed, size_t seed_len) {
    static const uint8_t ns[] = "zmk-raw-hid-device-uid-v1";
    uint64_t h = FNV1A_64_OFFSET_BASIS;

    /* Feed namespace (excluding NUL terminator) then seed into one FNV-1a stream. */
    for (size_t i = 0; i < sizeof(ns) - 1; i++) {
        h ^= ns[i];
        h *= FNV1A_64_PRIME;
    }
    for (size_t i = 0; i < seed_len; i++) {
        h ^= seed[i];
        h *= FNV1A_64_PRIME;
    }

    /* Per spec: hash==0 is reserved as "invalid"; map it to 1. */
    return h == 0 ? 1 : h;
}

/* --- Settings branch -------------------------------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)

#define IDENTITY_SEED_SIZE 16

static uint8_t identity_seed[IDENTITY_SEED_SIZE];
static bool identity_seed_loaded;

static int identity_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "identity_seed", &next) && !next) {
        if (len != IDENTITY_SEED_SIZE) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, identity_seed, IDENTITY_SEED_SIZE);
        if (rc >= 0) {
            identity_seed_loaded = true;
        }
        return MIN(rc, 0);
    }

    return -ENOENT;
}

static int identity_settings_commit(void) {
    if (uid_hash != 0) {
        /* Already resolved by hwinfo; settings branch is a no-op. */
        return 0;
    }

    if (identity_seed_loaded) {
        uid_hash = compute_uid_hash(identity_seed, IDENTITY_SEED_SIZE);
        LOG_INF("rawhid identity: uid_hash resolved from settings seed");
        return 0;
    }

    /* First boot: generate a random seed and persist it. */
    uint8_t new_seed[IDENTITY_SEED_SIZE];
    sys_rand_get(new_seed, IDENTITY_SEED_SIZE);

    int rc = settings_save_one("raw_hid/identity_seed", new_seed, IDENTITY_SEED_SIZE);
    if (rc == 0) {
        uid_hash = compute_uid_hash(new_seed, IDENTITY_SEED_SIZE);
        LOG_INF("rawhid identity: uid_hash resolved from new random seed");
    } else {
        LOG_WRN("rawhid identity: failed to save identity seed (%d); uid_hash=0", rc);
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rawhid_app_identity, "raw_hid", NULL, identity_settings_set,
                               identity_settings_commit, NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* --- SYS_INIT --------------------------------------------------------------- */

static int identity_init(void) {
#if IS_ENABLED(CONFIG_HWINFO)
    uint8_t id_buf[32];
    ssize_t id_len = hwinfo_get_device_id(id_buf, sizeof(id_buf));

    if (id_len > 0) {
        uid_hash = compute_uid_hash(id_buf, (size_t)id_len);
        LOG_INF("rawhid identity: uid_hash resolved from hwinfo");
        return 0;
    }
    LOG_DBG("rawhid identity: hwinfo unavailable (len=%zd), trying settings", (ssize_t)id_len);
#endif

    /* Settings branch is handled by identity_settings_commit(), called during
     * settings_load() in main(). If CONFIG_SETTINGS is disabled and CONFIG_HWINFO
     * is also disabled (or returned no data), uid_hash remains 0 — identity unavailable. */

#if !IS_ENABLED(CONFIG_HWINFO) && !IS_ENABLED(CONFIG_SETTINGS)
    LOG_WRN("rawhid identity: neither hwinfo nor settings enabled; uid_hash=0 (unavailable)");
#endif

    return 0;
}

SYS_INIT(identity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* --- Public API ------------------------------------------------------------- */

uint64_t rawhid_app_identity_get_uid_hash(void) { return uid_hash; }

uint32_t rawhid_app_identity_get_capabilities(void) {
    uint32_t caps = 0;

    if (IS_ENABLED(CONFIG_RAWHID_APP_LAYER_CONTROL)) {
        caps |= BIT(0);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_TIME_SYNC)) {
        caps |= BIT(1);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_AI_USAGE)) {
        caps |= BIT(2);
    }
    /* bit 3 (THEME): no CONFIG yet, always 0 */
    if (IS_ENABLED(CONFIG_RAWHID_APP_BATTERY_REPORT)) {
        caps |= BIT(4);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_HOST_ACTION)) {
        caps |= BIT(5);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_KEY_STATS)) {
        caps |= BIT(6);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_LAYER_STATE_REPORT)) {
        caps |= BIT(7);
    }
    if (IS_ENABLED(CONFIG_RAWHID_APP_KEY_PRESS)) {
        caps |= BIT(8);
    }

    return caps;
}
