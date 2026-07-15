#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include <rawhid_app/encoder_runtime.h>
#include <rawhid_app/behavior_identity.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS MAX(ZMK_KEYMAP_SENSORS_LEN, 1)
#define RAWHID_APP_ENCODER_RUNTIME_ENTRY_SLOTS                                               \
    (2 * ZMK_KEYMAP_LAYERS_LEN * RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS)

#define RAWHID_APP_ENCODER_SOURCE_KEYMAP 0x00
#define RAWHID_APP_ENCODER_SOURCE_OVERRIDE 0x01
#define RAWHID_APP_ENCODER_FLAG_STALE_SAVED_EXISTS BIT(0)
#define RAWHID_APP_ENCODER_FLAG_SAVED_EXISTS BIT(1)
#define RAWHID_APP_ENCODER_FLAG_RUNTIME_DIRTY BIT(2)
#define RAWHID_APP_ENCODER_FLAG_INVALID_SAVED_EXISTS BIT(3)

#define RAWHID_APP_ENCODER_INVALID_BEHAVIOR_ID UINT16_MAX

#define RAWHID_APP_ENCODER_SETTINGS_SUBTREE "keylink/enc/v1"
#define RAWHID_APP_ENCODER_SETTINGS_KEY_MAX_LEN 40
#define RAWHID_APP_ENCODER_RECORD_LEN 64
#define RAWHID_APP_ENCODER_RECORD_CRC_LEN 60
#define RAWHID_APP_ENCODER_RECORD_MAGIC_0 'K'
#define RAWHID_APP_ENCODER_RECORD_MAGIC_1 'E'
#define RAWHID_APP_ENCODER_RECORD_VERSION 1
#define RAWHID_APP_ENCODER_RECORD_HASH_LEN 16
#define RAWHID_APP_ENCODER_POINTER_MOVE_DIVISOR 20
#define RAWHID_APP_ENCODER_SCROLL_DETENTS_PER_NOTCH 2

#define RAWHID_APP_ENCODER_RECORD_MAGIC 0
#define RAWHID_APP_ENCODER_RECORD_VERSION_OFFSET 2
#define RAWHID_APP_ENCODER_RECORD_FLAGS 3
#define RAWHID_APP_ENCODER_RECORD_HASH_LEN_OFFSET 4
#define RAWHID_APP_ENCODER_RECORD_RESERVED 5
#define RAWHID_APP_ENCODER_RECORD_CW_HASH 8
#define RAWHID_APP_ENCODER_RECORD_CCW_HASH 24
#define RAWHID_APP_ENCODER_RECORD_CW_BINDING 40
#define RAWHID_APP_ENCODER_RECORD_CCW_BINDING 50
#define RAWHID_APP_ENCODER_RECORD_CRC 60

#define RAWHID_APP_ENCODER_BINDING_LEN 10
#define RAWHID_APP_ENCODER_BINDING_BEHAVIOR_ID 0
#define RAWHID_APP_ENCODER_BINDING_PARAM1 2
#define RAWHID_APP_ENCODER_BINDING_PARAM2 6

enum rawhid_app_encoder_direction {
    RAWHID_APP_ENCODER_DIRECTION_NONE = 0,
    RAWHID_APP_ENCODER_DIRECTION_CW,
    RAWHID_APP_ENCODER_DIRECTION_CCW,
};

enum rawhid_app_encoder_saved_state {
    RAWHID_APP_ENCODER_SAVED_NONE = 0,
    RAWHID_APP_ENCODER_SAVED_VALID,
    RAWHID_APP_ENCODER_SAVED_STALE,
    RAWHID_APP_ENCODER_SAVED_INVALID,
};

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_input_two_axis)
struct rawhid_app_encoder_two_axis_target {
    const struct device *dev;
    uint16_t x_code;
    uint16_t y_code;
};

#define RAWHID_APP_ENCODER_TWO_AXIS_TARGET(node_id)                                                  \
    {                                                                                                \
        .dev = DEVICE_DT_GET(node_id),                                                               \
        .x_code = DT_PROP(node_id, x_input_code),                                                    \
        .y_code = DT_PROP(node_id, y_input_code),                                                    \
    },

static const struct rawhid_app_encoder_two_axis_target encoder_two_axis_targets[] = {
    DT_FOREACH_STATUS_OKAY(zmk_behavior_input_two_axis, RAWHID_APP_ENCODER_TWO_AXIS_TARGET)};
#endif

struct rawhid_app_encoder_entry {
    bool occupied;
    uint32_t layer_id;
    uint8_t encoder_id;
    struct zmk_behavior_binding runtime_cw_binding;
    struct zmk_behavior_binding runtime_ccw_binding;
    struct zmk_behavior_binding saved_cw_binding;
    struct zmk_behavior_binding saved_ccw_binding;
    bool runtime_valid;
    bool runtime_dirty;
    bool delete_pending;
    bool pending_saved_record;
    uint8_t saved_record[RAWHID_APP_ENCODER_RECORD_LEN];
    enum rawhid_app_encoder_saved_state saved_state;
    struct sensor_value sensor_remainder;
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_input_two_axis)
    int16_t two_axis_x_remainder;
    int16_t two_axis_y_remainder;
#endif
};

static struct rawhid_app_encoder_entry encoder_entries[RAWHID_APP_ENCODER_RUNTIME_ENTRY_SLOTS];

static uint8_t encoder_save_record_buf[RAWHID_APP_ENCODER_RECORD_LEN] __aligned(4);
static struct rawhid_app_encoder_entry
    *encoder_save_dirty_entries[RAWHID_APP_ENCODER_RUNTIME_ENTRY_SLOTS];
static size_t encoder_save_dirty_entry_count;
static bool encoder_runtime_settings_committed;

static bool encoder_runtime_layer_id_to_index(uint32_t layer_id, uint8_t *layer_index) {
    if (layer_id > UINT8_MAX) {
        return false;
    }

    for (uint8_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        zmk_keymap_layer_id_t candidate = zmk_keymap_layer_index_to_id(index);
        if (candidate != ZMK_KEYMAP_LAYER_ID_INVAL && candidate == (zmk_keymap_layer_id_t)layer_id) {
            if (layer_index != NULL) {
                *layer_index = index;
            }
            return true;
        }
    }

    return false;
}

bool rawhid_app_encoder_runtime_layer_exists(uint32_t layer_id) {
    return encoder_runtime_layer_id_to_index(layer_id, NULL);
}

static struct rawhid_app_encoder_entry *encoder_runtime_find_entry(uint32_t layer_id,
                                                                   uint8_t encoder_id) {
    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (entry->occupied && entry->layer_id == layer_id && entry->encoder_id == encoder_id) {
            return entry;
        }
    }

    return NULL;
}

static struct rawhid_app_encoder_entry *encoder_runtime_allocate_entry(uint32_t layer_id,
                                                                       uint8_t encoder_id) {
    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (!entry->occupied) {
            memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->layer_id = layer_id;
            entry->encoder_id = encoder_id;
            return entry;
        }
    }

    LOG_ERR("encoder runtime slot pool full layer_id=%u encoder_id=%u capacity=%u", layer_id,
            encoder_id, (unsigned int)ARRAY_SIZE(encoder_entries));
    return NULL;
}

static struct rawhid_app_encoder_entry *
encoder_runtime_find_or_allocate_entry(uint32_t layer_id, uint8_t encoder_id) {
    struct rawhid_app_encoder_entry *entry = encoder_runtime_find_entry(layer_id, encoder_id);
    return entry != NULL ? entry : encoder_runtime_allocate_entry(layer_id, encoder_id);
}

static void encoder_runtime_release_entry(struct rawhid_app_encoder_entry *entry) {
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
}

static struct rawhid_app_encoder_entry *encoder_runtime_entry_by_index(uint8_t layer_index,
                                                                       uint8_t encoder_id) {
    if (layer_index >= ZMK_KEYMAP_LAYERS_LEN || encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return NULL;
    }

    zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);
    if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return NULL;
    }

    return encoder_runtime_find_entry(layer_id, encoder_id);
}

static struct rawhid_app_encoder_entry *encoder_runtime_entry(uint32_t layer_id,
                                                              uint8_t encoder_id) {
    if (!rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return NULL;
    }

    return encoder_runtime_find_entry(layer_id, encoder_id);
}

static bool encoder_runtime_bindings_equal(const struct zmk_behavior_binding *a,
                                           const struct zmk_behavior_binding *b) {
    if (a->behavior_dev == NULL || b->behavior_dev == NULL) {
        return a->behavior_dev == b->behavior_dev && a->param1 == b->param1 &&
               a->param2 == b->param2;
    }

    return strcmp(a->behavior_dev, b->behavior_dev) == 0 && a->param1 == b->param1 &&
           a->param2 == b->param2;
}

static bool encoder_runtime_matches_saved(const struct rawhid_app_encoder_entry *entry) {
    return entry->saved_state == RAWHID_APP_ENCODER_SAVED_VALID && entry->runtime_valid &&
           encoder_runtime_bindings_equal(&entry->runtime_cw_binding, &entry->saved_cw_binding) &&
           encoder_runtime_bindings_equal(&entry->runtime_ccw_binding, &entry->saved_ccw_binding);
}

static void encoder_runtime_reset_accumulators(struct rawhid_app_encoder_entry *entry) {
    entry->sensor_remainder = (struct sensor_value){0};
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_input_two_axis)
    entry->two_axis_x_remainder = 0;
    entry->two_axis_y_remainder = 0;
#endif
}

static uint8_t encoder_runtime_flags(const struct rawhid_app_encoder_entry *entry) {
    uint8_t flags = 0;

    if (entry->runtime_valid) {
        if (entry->saved_state == RAWHID_APP_ENCODER_SAVED_VALID) {
            flags |= RAWHID_APP_ENCODER_FLAG_SAVED_EXISTS;
        }
        if (entry->runtime_dirty) {
            flags |= RAWHID_APP_ENCODER_FLAG_RUNTIME_DIRTY;
        }
        return flags;
    }

    switch (entry->saved_state) {
    case RAWHID_APP_ENCODER_SAVED_VALID:
        flags |= RAWHID_APP_ENCODER_FLAG_SAVED_EXISTS;
        break;
    case RAWHID_APP_ENCODER_SAVED_STALE:
        flags |= RAWHID_APP_ENCODER_FLAG_STALE_SAVED_EXISTS;
        break;
    case RAWHID_APP_ENCODER_SAVED_INVALID:
        flags |= RAWHID_APP_ENCODER_FLAG_INVALID_SAVED_EXISTS;
        break;
    default:
        break;
    }

    if (entry->runtime_dirty) {
        flags |= RAWHID_APP_ENCODER_FLAG_RUNTIME_DIRTY;
    }

    return flags;
}

bool rawhid_app_encoder_runtime_get(uint32_t layer_id, uint8_t encoder_id,
                                    struct rawhid_app_encoder_runtime_bindings *bindings) {
    if (bindings == NULL || !rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return false;
    }

    memset(bindings, 0, sizeof(*bindings));
    bindings->source = RAWHID_APP_ENCODER_SOURCE_KEYMAP;

    struct rawhid_app_encoder_entry *entry = encoder_runtime_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return true;
    }

    bindings->flags = encoder_runtime_flags(entry);

    if (!entry->runtime_valid) {
        return true;
    }

    bindings->source = RAWHID_APP_ENCODER_SOURCE_OVERRIDE;
    bindings->cw_binding = entry->runtime_cw_binding;
    bindings->ccw_binding = entry->runtime_ccw_binding;
    return true;
}

int rawhid_app_encoder_runtime_set(uint32_t layer_id, uint8_t encoder_id,
                                   const struct zmk_behavior_binding *cw_binding,
                                   const struct zmk_behavior_binding *ccw_binding) {
    if (cw_binding == NULL || ccw_binding == NULL ||
        !rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return -EINVAL;
    }

    struct rawhid_app_encoder_entry *entry =
        encoder_runtime_find_or_allocate_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return -ENOMEM;
    }

    entry->runtime_cw_binding = *cw_binding;
    entry->runtime_ccw_binding = *ccw_binding;
    entry->runtime_valid = true;
    entry->delete_pending = false;
    entry->runtime_dirty = !encoder_runtime_matches_saved(entry);
    encoder_runtime_reset_accumulators(entry);
    return 0;
}

bool rawhid_app_encoder_runtime_dirty(void) {
    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (!entry->occupied) {
            continue;
        }

        bool orphan_record = !rawhid_app_encoder_runtime_layer_exists(entry->layer_id) &&
                             (entry->saved_state != RAWHID_APP_ENCODER_SAVED_NONE ||
                              entry->pending_saved_record);
        if (entry->runtime_dirty || entry->delete_pending || orphan_record) {
            return true;
        }
    }

    return false;
}

static void encoder_runtime_encode_binding(const struct zmk_behavior_binding *binding,
                                           uint8_t *data) {
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);

    sys_put_le16(behavior_id, &data[RAWHID_APP_ENCODER_BINDING_BEHAVIOR_ID]);
    sys_put_le32(binding->param1, &data[RAWHID_APP_ENCODER_BINDING_PARAM1]);
    sys_put_le32(binding->param2, &data[RAWHID_APP_ENCODER_BINDING_PARAM2]);
}

static bool encoder_runtime_decode_binding(const uint8_t *data, const char *direction,
                                           struct zmk_behavior_binding *binding) {
    ARG_UNUSED(direction);

    uint16_t behavior_id = sys_get_le16(&data[RAWHID_APP_ENCODER_BINDING_BEHAVIOR_ID]);
    if (behavior_id == RAWHID_APP_ENCODER_INVALID_BEHAVIOR_ID) {
        return false;
    }

    const char *behavior_dev = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (behavior_dev == NULL) {
        return false;
    }

    *binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_dev,
        .param1 = sys_get_le32(&data[RAWHID_APP_ENCODER_BINDING_PARAM1]),
        .param2 = sys_get_le32(&data[RAWHID_APP_ENCODER_BINDING_PARAM2]),
    };
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    binding->local_id = behavior_id;
#endif

    int rc = zmk_behavior_validate_binding(binding);
    if (rc < 0) {
        return rc == -ENODEV && zmk_behavior_get_binding(behavior_dev) != NULL;
    }

    return true;
}

static void encoder_runtime_identity_hash(const struct zmk_behavior_binding *binding,
                                          uint8_t hash[RAWHID_APP_ENCODER_RECORD_HASH_LEN]) {
    rawhid_app_behavior_identity_hash(binding, hash, RAWHID_APP_ENCODER_RECORD_HASH_LEN);
}

static bool encoder_runtime_hash_matches(const struct zmk_behavior_binding *binding,
                                         const uint8_t *expected_hash) {
    uint8_t actual_hash[RAWHID_APP_ENCODER_RECORD_HASH_LEN];
    encoder_runtime_identity_hash(binding, actual_hash);
    return memcmp(actual_hash, expected_hash, RAWHID_APP_ENCODER_RECORD_HASH_LEN) == 0;
}

static int encoder_runtime_settings_key(const struct rawhid_app_encoder_entry *entry, char *key,
                                        size_t key_size) {
    int ret = snprintf(key, key_size, "%s/l%04x/e%04x", RAWHID_APP_ENCODER_SETTINGS_SUBTREE,
                       (unsigned int)entry->layer_id, entry->encoder_id);
    if (ret < 0 || (size_t)ret >= key_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static void encoder_runtime_build_record(const struct rawhid_app_encoder_entry *entry,
                                         uint8_t record[RAWHID_APP_ENCODER_RECORD_LEN]) {
    memset(record, 0, RAWHID_APP_ENCODER_RECORD_LEN);
    record[RAWHID_APP_ENCODER_RECORD_MAGIC] = RAWHID_APP_ENCODER_RECORD_MAGIC_0;
    record[RAWHID_APP_ENCODER_RECORD_MAGIC + 1] = RAWHID_APP_ENCODER_RECORD_MAGIC_1;
    record[RAWHID_APP_ENCODER_RECORD_VERSION_OFFSET] = RAWHID_APP_ENCODER_RECORD_VERSION;
    record[RAWHID_APP_ENCODER_RECORD_HASH_LEN_OFFSET] = RAWHID_APP_ENCODER_RECORD_HASH_LEN;

    encoder_runtime_identity_hash(&entry->runtime_cw_binding,
                                  &record[RAWHID_APP_ENCODER_RECORD_CW_HASH]);
    encoder_runtime_identity_hash(&entry->runtime_ccw_binding,
                                  &record[RAWHID_APP_ENCODER_RECORD_CCW_HASH]);
    encoder_runtime_encode_binding(&entry->runtime_cw_binding,
                                   &record[RAWHID_APP_ENCODER_RECORD_CW_BINDING]);
    encoder_runtime_encode_binding(&entry->runtime_ccw_binding,
                                   &record[RAWHID_APP_ENCODER_RECORD_CCW_BINDING]);

    uint32_t crc = crc32_ieee(record, RAWHID_APP_ENCODER_RECORD_CRC_LEN);
    sys_put_le32(crc, &record[RAWHID_APP_ENCODER_RECORD_CRC]);
}

static bool encoder_runtime_record_header_valid(const uint8_t *record) {
    return record[RAWHID_APP_ENCODER_RECORD_MAGIC] == RAWHID_APP_ENCODER_RECORD_MAGIC_0 &&
           record[RAWHID_APP_ENCODER_RECORD_MAGIC + 1] == RAWHID_APP_ENCODER_RECORD_MAGIC_1 &&
           record[RAWHID_APP_ENCODER_RECORD_VERSION_OFFSET] == RAWHID_APP_ENCODER_RECORD_VERSION &&
           record[RAWHID_APP_ENCODER_RECORD_FLAGS] == 0 &&
           record[RAWHID_APP_ENCODER_RECORD_HASH_LEN_OFFSET] ==
               RAWHID_APP_ENCODER_RECORD_HASH_LEN &&
           record[RAWHID_APP_ENCODER_RECORD_RESERVED] == 0 &&
           record[RAWHID_APP_ENCODER_RECORD_RESERVED + 1] == 0 &&
           record[RAWHID_APP_ENCODER_RECORD_RESERVED + 2] == 0;
}

static enum rawhid_app_encoder_saved_state
encoder_runtime_decode_record(const uint8_t record[RAWHID_APP_ENCODER_RECORD_LEN],
                              struct zmk_behavior_binding *cw_binding,
                              struct zmk_behavior_binding *ccw_binding) {
    if (!encoder_runtime_record_header_valid(record)) {
        return RAWHID_APP_ENCODER_SAVED_INVALID;
    }

    uint32_t expected_crc = sys_get_le32(&record[RAWHID_APP_ENCODER_RECORD_CRC]);
    uint32_t actual_crc = crc32_ieee(record, RAWHID_APP_ENCODER_RECORD_CRC_LEN);
    if (expected_crc != actual_crc) {
        return RAWHID_APP_ENCODER_SAVED_INVALID;
    }

    bool cw_decoded =
        encoder_runtime_decode_binding(&record[RAWHID_APP_ENCODER_RECORD_CW_BINDING], "CW",
                                       cw_binding);
    bool ccw_decoded =
        encoder_runtime_decode_binding(&record[RAWHID_APP_ENCODER_RECORD_CCW_BINDING], "CCW",
                                       ccw_binding);
    if (!cw_decoded || !ccw_decoded) {
        return RAWHID_APP_ENCODER_SAVED_STALE;
    }

    bool cw_hash_matches =
        encoder_runtime_hash_matches(cw_binding, &record[RAWHID_APP_ENCODER_RECORD_CW_HASH]);
    bool ccw_hash_matches =
        encoder_runtime_hash_matches(ccw_binding, &record[RAWHID_APP_ENCODER_RECORD_CCW_HASH]);
    if (!cw_hash_matches || !ccw_hash_matches) {
        return RAWHID_APP_ENCODER_SAVED_STALE;
    }

    return RAWHID_APP_ENCODER_SAVED_VALID;
}

static bool encoder_runtime_binding_has_local_id(const struct zmk_behavior_binding *binding) {
    return zmk_behavior_get_local_id(binding->behavior_dev) != RAWHID_APP_ENCODER_INVALID_BEHAVIOR_ID;
}

static void encoder_runtime_finalize_saved_entry(struct rawhid_app_encoder_entry *entry);

static int encoder_runtime_persist_entry(struct rawhid_app_encoder_entry *entry) {
    char key[RAWHID_APP_ENCODER_SETTINGS_KEY_MAX_LEN];
    int rc = encoder_runtime_settings_key(entry, key, sizeof(key));
    if (rc < 0) {
        return rc;
    }

    if (!rawhid_app_encoder_runtime_layer_exists(entry->layer_id)) {
        if (!encoder_runtime_settings_committed) {
            LOG_WRN("encoder tombstone delete blocked before settings load commit key=%s", key);
            return -EIO;
        }

        rc = settings_delete(key);
        if (rc < 0) {
            LOG_WRN("encoder tombstone delete failed key=%s err=%d", key, rc);
        }
        return rc;
    }

    if (entry->runtime_valid) {
        if (!encoder_runtime_binding_has_local_id(&entry->runtime_cw_binding) ||
            !encoder_runtime_binding_has_local_id(&entry->runtime_ccw_binding)) {
            LOG_WRN("encoder override save failed key=%s missing behavior local id", key);
            return -EINVAL;
        }

        encoder_runtime_build_record(entry, encoder_save_record_buf);
        rc = settings_save_one(key, encoder_save_record_buf, sizeof(encoder_save_record_buf));
        if (rc < 0) {
            LOG_WRN("encoder override save failed key=%s err=%d", key, rc);
            return rc;
        }

        return 0;
    }

    if (entry->delete_pending) {
        if (!encoder_runtime_settings_committed) {
            LOG_WRN("encoder override delete blocked before settings load commit key=%s", key);
            return -EIO;
        }

        rc = settings_delete(key);
        if (rc < 0) {
            LOG_WRN("encoder override delete failed key=%s err=%d", key, rc);
            return rc;
        }
    }

    return 0;
}

static void encoder_runtime_finalize_saved_entry(struct rawhid_app_encoder_entry *entry) {
    if (entry->runtime_valid) {
        entry->saved_cw_binding = entry->runtime_cw_binding;
        entry->saved_ccw_binding = entry->runtime_ccw_binding;
        entry->saved_state = RAWHID_APP_ENCODER_SAVED_VALID;
    } else if (entry->delete_pending || entry->runtime_dirty) {
        memset(&entry->saved_cw_binding, 0, sizeof(entry->saved_cw_binding));
        memset(&entry->saved_ccw_binding, 0, sizeof(entry->saved_ccw_binding));
        entry->saved_state = RAWHID_APP_ENCODER_SAVED_NONE;
    }

    entry->runtime_dirty = false;
    entry->delete_pending = false;
}

int rawhid_app_encoder_runtime_save(void) {
    encoder_save_dirty_entry_count = 0;
    memset(encoder_save_dirty_entries, 0, sizeof(encoder_save_dirty_entries));

    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (!entry->occupied) {
            continue;
        }

        bool tombstone = !rawhid_app_encoder_runtime_layer_exists(entry->layer_id);
        if (!tombstone && !entry->runtime_dirty && !entry->delete_pending) {
            continue;
        }

        encoder_save_dirty_entries[encoder_save_dirty_entry_count++] = entry;
    }

    for (size_t index = 0; index < encoder_save_dirty_entry_count; index++) {
        int rc = encoder_runtime_persist_entry(encoder_save_dirty_entries[index]);
        if (rc < 0) {
            memset(encoder_save_dirty_entries, 0, sizeof(encoder_save_dirty_entries));
            encoder_save_dirty_entry_count = 0;
            return rc;
        }
    }

    for (size_t index = 0; index < encoder_save_dirty_entry_count; index++) {
        struct rawhid_app_encoder_entry *entry = encoder_save_dirty_entries[index];
        if (rawhid_app_encoder_runtime_layer_exists(entry->layer_id)) {
            encoder_runtime_finalize_saved_entry(entry);
        } else {
            encoder_runtime_release_entry(entry);
        }
    }

    memset(encoder_save_dirty_entries, 0, sizeof(encoder_save_dirty_entries));
    encoder_save_dirty_entry_count = 0;
    return 0;
}

void rawhid_app_encoder_runtime_discard(void) {
    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (!entry->occupied) {
            continue;
        }

        if (!rawhid_app_encoder_runtime_layer_exists(entry->layer_id)) {
            if (entry->saved_state == RAWHID_APP_ENCODER_SAVED_NONE &&
                !entry->pending_saved_record) {
                encoder_runtime_release_entry(entry);
            } else {
                entry->runtime_valid = false;
                entry->runtime_dirty = true;
                entry->delete_pending = false;
                encoder_runtime_reset_accumulators(entry);
            }
            continue;
        }

        if (entry->saved_state == RAWHID_APP_ENCODER_SAVED_VALID) {
            entry->runtime_cw_binding = entry->saved_cw_binding;
            entry->runtime_ccw_binding = entry->saved_ccw_binding;
            entry->runtime_valid = true;
        } else {
            memset(&entry->runtime_cw_binding, 0, sizeof(entry->runtime_cw_binding));
            memset(&entry->runtime_ccw_binding, 0, sizeof(entry->runtime_ccw_binding));
            entry->runtime_valid = false;
        }
        encoder_runtime_reset_accumulators(entry);
        entry->runtime_dirty = false;
        entry->delete_pending = false;
    }
}

void rawhid_app_encoder_runtime_clear(uint32_t layer_id, uint8_t encoder_id) {
    struct rawhid_app_encoder_entry *entry = encoder_runtime_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return;
    }

    bool had_any_state = entry->runtime_valid ||
                         entry->saved_state != RAWHID_APP_ENCODER_SAVED_NONE;
    if (!had_any_state) {
        return;
    }

    memset(&entry->runtime_cw_binding, 0, sizeof(entry->runtime_cw_binding));
    memset(&entry->runtime_ccw_binding, 0, sizeof(entry->runtime_ccw_binding));
    entry->runtime_valid = false;
    entry->runtime_dirty = true;
    entry->delete_pending = entry->saved_state != RAWHID_APP_ENCODER_SAVED_NONE;
    encoder_runtime_reset_accumulators(entry);
}

static bool encoder_runtime_parse_hex_component(const char *component, char prefix,
                                                uint32_t *value) {
    if (component == NULL || component[0] != prefix || component[1] == '\0') {
        return false;
    }

    char *endptr;
    unsigned long parsed = strtoul(&component[1], &endptr, 16);
    if (*endptr != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool encoder_runtime_parse_settings_name(const char *name, uint32_t *layer_id,
                                                uint8_t *encoder_id) {
    const char *separator = strchr(name, SETTINGS_NAME_SEPARATOR);
    if (separator == NULL) {
        return false;
    }

    char layer_component[16];
    size_t layer_len = separator - name;
    if (layer_len == 0 || layer_len >= sizeof(layer_component)) {
        return false;
    }
    memcpy(layer_component, name, layer_len);
    layer_component[layer_len] = '\0';

    uint32_t parsed_layer;
    uint32_t parsed_encoder;
    if (!encoder_runtime_parse_hex_component(layer_component, 'l', &parsed_layer) ||
        !encoder_runtime_parse_hex_component(separator + 1, 'e', &parsed_encoder) ||
        parsed_encoder > UINT8_MAX) {
        return false;
    }

    *layer_id = parsed_layer;
    *encoder_id = (uint8_t)parsed_encoder;
    return true;
}

static int encoder_runtime_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                        void *cb_arg) {
    uint32_t layer_id;
    uint8_t encoder_id;
    if (!encoder_runtime_parse_settings_name(name, &layer_id, &encoder_id)) {
        return 0;
    }

    if (encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        LOG_WRN("encoder override ignored for invalid encoder_id=%u layer_id=%u", encoder_id,
                layer_id);
        return 0;
    }

    struct rawhid_app_encoder_entry *entry =
        encoder_runtime_find_or_allocate_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return -ENOMEM;
    }

    if (len == 0) {
        encoder_runtime_release_entry(entry);
        return 0;
    }

    if (len != RAWHID_APP_ENCODER_RECORD_LEN) {
        entry->saved_state = RAWHID_APP_ENCODER_SAVED_INVALID;
        entry->pending_saved_record = false;
        return 0;
    }

    int rc = read_cb(cb_arg, entry->saved_record, sizeof(entry->saved_record));
    if (rc != (int)sizeof(entry->saved_record)) {
        LOG_WRN("encoder override read failed layer_id=%u encoder_id=%u err=%d", layer_id,
                encoder_id, rc);
        entry->saved_state = RAWHID_APP_ENCODER_SAVED_INVALID;
        entry->pending_saved_record = false;
        return 0;
    }

    entry->pending_saved_record = true;
    entry->runtime_dirty = false;
    entry->delete_pending = false;
    return 0;
}

static int encoder_runtime_settings_commit(void) {
    encoder_runtime_settings_committed = true;

    for (size_t slot = 0; slot < ARRAY_SIZE(encoder_entries); slot++) {
        struct rawhid_app_encoder_entry *entry = &encoder_entries[slot];
        if (!entry->occupied || !entry->pending_saved_record) {
            continue;
        }

        struct zmk_behavior_binding cw_binding;
        struct zmk_behavior_binding ccw_binding;
        enum rawhid_app_encoder_saved_state saved_state = encoder_runtime_decode_record(
            entry->saved_record, &cw_binding, &ccw_binding);

        entry->saved_state = saved_state;
        bool layer_exists = rawhid_app_encoder_runtime_layer_exists(entry->layer_id);
        if (saved_state == RAWHID_APP_ENCODER_SAVED_VALID) {
            entry->saved_cw_binding = cw_binding;
            entry->saved_ccw_binding = ccw_binding;
            if (layer_exists) {
                entry->runtime_cw_binding = cw_binding;
                entry->runtime_ccw_binding = ccw_binding;
                entry->runtime_valid = true;
            } else {
                entry->runtime_valid = false;
            }
        } else {
            memset(&entry->saved_cw_binding, 0, sizeof(entry->saved_cw_binding));
            memset(&entry->saved_ccw_binding, 0, sizeof(entry->saved_ccw_binding));
            entry->runtime_valid = false;
        }
        encoder_runtime_reset_accumulators(entry);

        entry->runtime_dirty = !layer_exists;
        entry->delete_pending = false;
        entry->pending_saved_record = false;
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(rawhid_app_encoder_runtime,
                                          RAWHID_APP_ENCODER_SETTINGS_SUBTREE, NULL,
                                          encoder_runtime_settings_set,
                                          encoder_runtime_settings_commit, NULL, 10);

static int encoder_runtime_signed_steps(const struct zmk_sensor_event *sensor_ev) {
    if (sensor_ev->channel_data_size == 0) {
        return 0;
    }

    const struct sensor_value value = sensor_ev->channel_data[0].value;
    if (value.val1 != 0) {
        return value.val1;
    }

    return value.val2;
}

static enum rawhid_app_encoder_direction encoder_runtime_direction_from_steps(int steps) {
    if (steps > 0) {
        return RAWHID_APP_ENCODER_DIRECTION_CW;
    }

    if (steps < 0) {
        return RAWHID_APP_ENCODER_DIRECTION_CCW;
    }

    return RAWHID_APP_ENCODER_DIRECTION_NONE;
}

static int encoder_runtime_triggers_from_sensor_event(struct rawhid_app_encoder_entry *entry,
                                                      const struct zmk_sensor_event *sensor_ev) {
    if (sensor_ev->channel_data_size == 0) {
        return 0;
    }

    const struct sensor_value value = sensor_ev->channel_data[0].value;

    /* Match ZMK's sensor rotate conversion so keyboard firmware settings decide
     * the physical detent-to-trigger mapping.
     */
    if (value.val1 == 0) {
        return value.val2;
    }

    const struct zmk_sensor_config *sensor_config =
        zmk_sensors_get_config_at_index(sensor_ev->sensor_index);
    if (sensor_config == NULL || sensor_config->triggers_per_rotation == 0 ||
        sensor_config->triggers_per_rotation > 360) {
        return encoder_runtime_signed_steps(sensor_ev);
    }

    entry->sensor_remainder.val1 += value.val1;
    entry->sensor_remainder.val2 += value.val2;

    if (abs(entry->sensor_remainder.val2) >= 1000000) {
        entry->sensor_remainder.val1 += entry->sensor_remainder.val2 / 1000000;
        entry->sensor_remainder.val2 %= 1000000;
    }

    int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
    int triggers = entry->sensor_remainder.val1 / trigger_degrees;
    entry->sensor_remainder.val1 %= trigger_degrees;

    return triggers;
}

static const char *encoder_runtime_direction_name(enum rawhid_app_encoder_direction direction) {
    switch (direction) {
    case RAWHID_APP_ENCODER_DIRECTION_CW:
        return "CW";
    case RAWHID_APP_ENCODER_DIRECTION_CCW:
        return "CCW";
    default:
        return "NONE";
    }
}

static const struct zmk_behavior_binding *
encoder_runtime_binding_for_direction(const struct rawhid_app_encoder_entry *entry,
                                      enum rawhid_app_encoder_direction direction) {
    switch (direction) {
    case RAWHID_APP_ENCODER_DIRECTION_CW:
        return &entry->runtime_cw_binding;
    case RAWHID_APP_ENCODER_DIRECTION_CCW:
        return &entry->runtime_ccw_binding;
    default:
        return NULL;
    }
}

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_input_two_axis)
static const struct rawhid_app_encoder_two_axis_target *
encoder_runtime_two_axis_target(const struct zmk_behavior_binding *binding) {
    if (binding == NULL || binding->behavior_dev == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < ARRAY_SIZE(encoder_two_axis_targets); i++) {
        const struct rawhid_app_encoder_two_axis_target *target = &encoder_two_axis_targets[i];
        if (device_is_ready(target->dev) && strcmp(binding->behavior_dev, target->dev->name) == 0) {
            return target;
        }
    }

    return NULL;
}

static int16_t encoder_runtime_decode_move_x(uint32_t param) {
    return (int16_t)((param >> 16) & 0xffff);
}

static int16_t encoder_runtime_decode_move_y(uint32_t param) {
    return (int16_t)(param & 0xffff);
}

static int16_t encoder_runtime_discrete_pointer_delta(uint16_t code, int16_t value,
                                                      uint8_t multiplier, int16_t *remainder) {
    if (value == 0 || multiplier == 0) {
        return 0;
    }

    int32_t delta;
    switch (code) {
    case INPUT_REL_WHEEL:
    case INPUT_REL_HWHEEL:
        if (remainder == NULL) {
            return 0;
        }
        *remainder += (value > 0 ? 1 : -1) * multiplier;
        delta = *remainder / RAWHID_APP_ENCODER_SCROLL_DETENTS_PER_NOTCH;
        *remainder %= RAWHID_APP_ENCODER_SCROLL_DETENTS_PER_NOTCH;
        return (int16_t)CLAMP(delta, INT16_MIN, INT16_MAX);
    default:
        delta = value / RAWHID_APP_ENCODER_POINTER_MOVE_DIVISOR;
        if (delta == 0) {
            delta = value > 0 ? 1 : -1;
        }
        break;
    }

    delta *= multiplier;
    return (int16_t)CLAMP(delta, INT16_MIN, INT16_MAX);
}

static bool encoder_runtime_report_two_axis(const struct zmk_behavior_binding *binding,
                                            struct rawhid_app_encoder_entry *entry, int steps) {
    const struct rawhid_app_encoder_two_axis_target *target =
        encoder_runtime_two_axis_target(binding);
    if (target == NULL) {
        return false;
    }

    uint8_t multiplier = (uint8_t)MIN(abs(steps), UINT8_MAX);
    int16_t x_value = encoder_runtime_decode_move_x(binding->param1);
    int16_t y_value = encoder_runtime_decode_move_y(binding->param1);
    int16_t x_delta =
        encoder_runtime_discrete_pointer_delta(target->x_code, x_value, multiplier,
                                               &entry->two_axis_x_remainder);
    int16_t y_delta =
        encoder_runtime_discrete_pointer_delta(target->y_code, y_value, multiplier,
                                               &entry->two_axis_y_remainder);

    bool have_x = x_delta != 0;
    bool have_y = y_delta != 0;
    int err = 0;

    if (have_x) {
        err = input_report_rel(target->dev, target->x_code, x_delta, !have_y, K_NO_WAIT);
        if (err < 0) {
            return true;
        }
    }

    if (have_y) {
        input_report_rel(target->dev, target->y_code, y_delta, true, K_NO_WAIT);
    }

    return true;
}
#endif

static int encoder_runtime_invoke_binding(const struct zmk_behavior_binding *binding,
                                          struct rawhid_app_encoder_entry *entry,
                                          uint8_t layer_id, uint8_t encoder_id,
                                          int steps, int64_t timestamp) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_input_two_axis)
    if (encoder_runtime_report_two_axis(binding, entry, steps)) {
        return 0;
    }
#endif

    struct zmk_behavior_binding_event event = {
        .layer = layer_id,
        .position = ZMK_VIRTUAL_KEY_POSITION_SENSOR(encoder_id),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int err = zmk_behavior_invoke_binding(binding, event, true);
    if (err < 0) {
        return err;
    }

    return zmk_behavior_invoke_binding(binding, event, false);
}

static int encoder_runtime_listener(const zmk_event_t *eh) {
    const struct zmk_sensor_event *sensor_ev = as_zmk_sensor_event(eh);
    if (sensor_ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t encoder_id = sensor_ev->sensor_index;
    int raw_steps = encoder_runtime_signed_steps(sensor_ev);
    zmk_keymap_layer_index_t active_layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t active_layer_id = zmk_keymap_layer_index_to_id(active_layer_index);

    if (encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        LOG_WRN("encoder runtime invalid sensor_index=%u layer_index=%u layer_id=%u steps=%d "
                "direction=%s",
                encoder_id, active_layer_index, active_layer_id, raw_steps,
                encoder_runtime_direction_name(encoder_runtime_direction_from_steps(raw_steps)));
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (active_layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        LOG_WRN("encoder runtime invalid active layer_index=%u layer_id=%u sensor_index=%u",
                active_layer_index, active_layer_id, encoder_id);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (encoder_runtime_direction_from_steps(raw_steps) == RAWHID_APP_ENCODER_DIRECTION_NONE) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct rawhid_app_encoder_entry *entry =
        encoder_runtime_entry_by_index(active_layer_index, encoder_id);
    if (entry == NULL || !entry->runtime_valid) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int triggers = encoder_runtime_triggers_from_sensor_event(entry, sensor_ev);
    enum rawhid_app_encoder_direction direction = encoder_runtime_direction_from_steps(triggers);

    LOG_INF("encoder runtime sensor_index=%u layer_index=%u layer_id=%u raw_steps=%d triggers=%d "
            "direction=%s",
            encoder_id, active_layer_index, active_layer_id, raw_steps, triggers,
            encoder_runtime_direction_name(direction));

    if (direction == RAWHID_APP_ENCODER_DIRECTION_NONE) {
        return ZMK_EV_EVENT_HANDLED;
    }

    const struct zmk_behavior_binding *binding =
        encoder_runtime_binding_for_direction(entry, direction);
    if (binding == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (uint8_t i = 0; i < MIN(abs(triggers), UINT8_MAX); i++) {
        int err = encoder_runtime_invoke_binding(binding, entry, active_layer_id, encoder_id,
                                                 triggers > 0 ? 1 : -1, sensor_ev->timestamp);
        if (err < 0) {
            LOG_WRN("encoder runtime invoke failed sensor_index=%u layer_id=%u direction=%s err=%d",
                    encoder_id, active_layer_id, encoder_runtime_direction_name(direction), err);
            return err;
        }
    }

    LOG_INF("encoder runtime handled sensor_index=%u layer_id=%u direction=%s", encoder_id,
            active_layer_id, encoder_runtime_direction_name(direction));

    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(rawhid_app_encoder_runtime, encoder_runtime_listener);
ZMK_SUBSCRIPTION(rawhid_app_encoder_runtime, zmk_sensor_event);
