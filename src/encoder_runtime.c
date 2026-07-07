#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <tinycrypt/sha256.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include <rawhid_app/encoder_runtime.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS MAX(ZMK_KEYMAP_SENSORS_LEN, 1)

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
#define RAWHID_APP_ENCODER_IDENTITY_SCHEMA_VERSION 1

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

struct rawhid_app_encoder_entry {
    uint32_t layer_id;
    uint8_t layer_index;
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
};

static struct rawhid_app_encoder_entry
    encoder_entries[ZMK_KEYMAP_LAYERS_LEN][RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS];

static uint8_t encoder_save_record_buf[RAWHID_APP_ENCODER_RECORD_LEN] __aligned(4);
static struct rawhid_app_encoder_entry
    *encoder_save_dirty_entries[ZMK_KEYMAP_LAYERS_LEN][RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS];

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

static struct rawhid_app_encoder_entry *encoder_runtime_entry_by_index(uint8_t layer_index,
                                                                       uint8_t encoder_id) {
    if (layer_index >= ZMK_KEYMAP_LAYERS_LEN || encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return NULL;
    }

    struct rawhid_app_encoder_entry *entry = &encoder_entries[layer_index][encoder_id];
    entry->layer_index = layer_index;
    entry->layer_id = zmk_keymap_layer_index_to_id(layer_index);
    entry->encoder_id = encoder_id;
    return entry;
}

static struct rawhid_app_encoder_entry *encoder_runtime_entry(uint32_t layer_id,
                                                              uint8_t encoder_id) {
    uint8_t layer_index;
    if (!encoder_runtime_layer_id_to_index(layer_id, &layer_index) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return NULL;
    }

    return encoder_runtime_entry_by_index(layer_index, encoder_id);
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
    if (bindings == NULL) {
        return false;
    }

    struct rawhid_app_encoder_entry *entry = encoder_runtime_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return false;
    }

    memset(bindings, 0, sizeof(*bindings));
    bindings->flags = encoder_runtime_flags(entry);

    if (!entry->runtime_valid) {
        bindings->source = RAWHID_APP_ENCODER_SOURCE_KEYMAP;
        return true;
    }

    bindings->source = RAWHID_APP_ENCODER_SOURCE_OVERRIDE;
    bindings->cw_binding = entry->runtime_cw_binding;
    bindings->ccw_binding = entry->runtime_ccw_binding;
    return true;
}

void rawhid_app_encoder_runtime_set(uint32_t layer_id, uint8_t encoder_id,
                                    const struct zmk_behavior_binding *cw_binding,
                                    const struct zmk_behavior_binding *ccw_binding) {
    if (cw_binding == NULL || ccw_binding == NULL) {
        return;
    }

    struct rawhid_app_encoder_entry *entry = encoder_runtime_entry(layer_id, encoder_id);
    if (entry == NULL) {
        return;
    }

    entry->runtime_cw_binding = *cw_binding;
    entry->runtime_ccw_binding = *ccw_binding;
    entry->runtime_valid = true;
    entry->delete_pending = false;
    entry->runtime_dirty = !encoder_runtime_matches_saved(entry);
}

bool rawhid_app_encoder_runtime_dirty(void) {
    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_runtime_entry_by_index(layer_index, encoder_id);
            if (entry != NULL && (entry->runtime_dirty || entry->delete_pending)) {
                return true;
            }
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
        return false;
    }

    return true;
}

static void encoder_runtime_hash_update_u8(struct tc_sha256_state_struct *sha, uint8_t value) {
    tc_sha256_update(sha, &value, sizeof(value));
}

static void encoder_runtime_hash_update_string(struct tc_sha256_state_struct *sha,
                                               const char *value) {
    uint16_t len = value == NULL ? 0 : (uint16_t)strlen(value);
    uint8_t len_bytes[2];
    sys_put_le16(len, len_bytes);
    tc_sha256_update(sha, len_bytes, sizeof(len_bytes));
    if (len > 0) {
        tc_sha256_update(sha, (const uint8_t *)value, len);
    }
}

static void encoder_runtime_identity_hash(const struct zmk_behavior_binding *binding,
                                          uint8_t hash[RAWHID_APP_ENCODER_RECORD_HASH_LEN]) {
    struct tc_sha256_state_struct sha;
    uint8_t digest[TC_SHA256_DIGEST_SIZE];

    tc_sha256_init(&sha);
    encoder_runtime_hash_update_u8(&sha, RAWHID_APP_ENCODER_IDENTITY_SCHEMA_VERSION);
    encoder_runtime_hash_update_string(&sha, binding->behavior_dev);
    encoder_runtime_hash_update_u8(&sha, 2);
    encoder_runtime_hash_update_string(&sha, "");
    encoder_runtime_hash_update_string(&sha, "");
    tc_sha256_final(digest, &sha);

    memcpy(hash, digest, RAWHID_APP_ENCODER_RECORD_HASH_LEN);
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
    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_runtime_entry_by_index(layer_index, encoder_id);
            if (entry == NULL || (!entry->runtime_dirty && !entry->delete_pending)) {
                continue;
            }

            encoder_save_dirty_entries[layer_index][encoder_id] = entry;
        }
    }

    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_save_dirty_entries[layer_index][encoder_id];
            if (entry == NULL) {
                continue;
            }

            int rc = encoder_runtime_persist_entry(entry);
            if (rc < 0) {
                memset(encoder_save_dirty_entries, 0, sizeof(encoder_save_dirty_entries));
                return rc;
            }
        }
    }

    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_save_dirty_entries[layer_index][encoder_id];
            if (entry != NULL) {
                encoder_runtime_finalize_saved_entry(entry);
            }
        }
    }

    memset(encoder_save_dirty_entries, 0, sizeof(encoder_save_dirty_entries));
    return 0;
}

void rawhid_app_encoder_runtime_discard(void) {
    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_runtime_entry_by_index(layer_index, encoder_id);
            if (entry == NULL) {
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
            entry->runtime_dirty = false;
            entry->delete_pending = false;
        }
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

    struct rawhid_app_encoder_entry *entry = encoder_runtime_entry(layer_id, encoder_id);
    if (entry == NULL) {
        LOG_WRN("encoder override ignored for unknown layer_id=%u encoder_id=%u", layer_id,
                encoder_id);
        return 0;
    }

    if (len == 0) {
        entry->saved_state = RAWHID_APP_ENCODER_SAVED_NONE;
        entry->pending_saved_record = false;
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
    for (uint8_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN; layer_index++) {
        for (uint8_t encoder_id = 0; encoder_id < ZMK_KEYMAP_SENSORS_LEN; encoder_id++) {
            struct rawhid_app_encoder_entry *entry =
                encoder_runtime_entry_by_index(layer_index, encoder_id);
            if (entry == NULL || !entry->pending_saved_record) {
                continue;
            }

            struct zmk_behavior_binding cw_binding;
            struct zmk_behavior_binding ccw_binding;
            enum rawhid_app_encoder_saved_state saved_state = encoder_runtime_decode_record(
                entry->saved_record, &cw_binding, &ccw_binding);

            entry->saved_state = saved_state;
            if (saved_state == RAWHID_APP_ENCODER_SAVED_VALID) {
                entry->saved_cw_binding = cw_binding;
                entry->saved_ccw_binding = ccw_binding;
                entry->runtime_cw_binding = cw_binding;
                entry->runtime_ccw_binding = ccw_binding;
                entry->runtime_valid = true;
            } else {
                memset(&entry->saved_cw_binding, 0, sizeof(entry->saved_cw_binding));
                memset(&entry->saved_ccw_binding, 0, sizeof(entry->saved_ccw_binding));
                entry->runtime_valid = false;
            }

            entry->runtime_dirty = false;
            entry->delete_pending = false;
            entry->pending_saved_record = false;
        }
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

static int encoder_runtime_invoke_binding(const struct zmk_behavior_binding *binding,
                                          uint8_t layer_id, uint8_t encoder_id,
                                          int64_t timestamp) {
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
    int steps = encoder_runtime_signed_steps(sensor_ev);
    enum rawhid_app_encoder_direction direction = encoder_runtime_direction_from_steps(steps);
    zmk_keymap_layer_index_t active_layer_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t active_layer_id = zmk_keymap_layer_index_to_id(active_layer_index);

    if (encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        LOG_WRN("encoder runtime invalid sensor_index=%u layer_index=%u layer_id=%u steps=%d "
                "direction=%s",
                encoder_id, active_layer_index, active_layer_id, steps,
                encoder_runtime_direction_name(direction));
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (active_layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        LOG_WRN("encoder runtime invalid active layer_index=%u layer_id=%u sensor_index=%u",
                active_layer_index, active_layer_id, encoder_id);
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("encoder runtime sensor_index=%u layer_index=%u layer_id=%u steps=%d direction=%s",
            encoder_id, active_layer_index, active_layer_id, steps,
            encoder_runtime_direction_name(direction));

    if (direction == RAWHID_APP_ENCODER_DIRECTION_NONE) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct rawhid_app_encoder_entry *entry =
        encoder_runtime_entry_by_index(active_layer_index, encoder_id);
    if (entry == NULL || !entry->runtime_valid) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_behavior_binding *binding =
        encoder_runtime_binding_for_direction(entry, direction);
    if (binding == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int err = encoder_runtime_invoke_binding(binding, active_layer_id, encoder_id,
                                             sensor_ev->timestamp);
    if (err < 0) {
        LOG_WRN("encoder runtime invoke failed sensor_index=%u layer_id=%u direction=%s err=%d",
                encoder_id, active_layer_id, encoder_runtime_direction_name(direction), err);
        return err;
    }

    LOG_INF("encoder runtime handled sensor_index=%u layer_id=%u direction=%s", encoder_id,
            active_layer_id, encoder_runtime_direction_name(direction));

    return ZMK_EV_EVENT_HANDLED;
}

ZMK_LISTENER(rawhid_app_encoder_runtime, encoder_runtime_listener);
ZMK_SUBSCRIPTION(rawhid_app_encoder_runtime, zmk_sensor_event);
