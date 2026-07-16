/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 Keylink Studio Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from zmk/app/src/combo.c at ZMK revision 484a0547078228f6957ed164523823e39fbedec4.
 * The event/candidate/release algorithm is intentionally kept structurally aligned with ZMK.
 * Keylink-specific changes are the disabled-DT default table, fixed 32-slot runtime table,
 * normalized positions, validation, and the read-only runtime API.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
#include <zephyr/settings/settings.h>
#endif

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <rawhid_app/combo_runtime.h>
#include <rawhid_app/behavior_identity.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RAWHID_APP_COMBO_DEFAULT_TIMEOUT_MS 50
#define RAWHID_APP_COMBO_SOURCE_ORDER_INVALID UINT8_MAX

#define RAWHID_APP_COMBO_SETTINGS_FLAGS_SAVED_TABLE_LOADED BIT(0)
#define RAWHID_APP_COMBO_SETTINGS_FLAGS_METADATA_INVALID_FALLBACK BIT(1)
#define RAWHID_APP_COMBO_SETTINGS_FLAGS_VERSION_UNSUPPORTED_FALLBACK BIT(2)
#define RAWHID_APP_COMBO_SETTINGS_FLAGS_READ_ERROR_FALLBACK BIT(3)
#define RAWHID_APP_COMBO_SETTINGS_FLAGS_STORAGE_UNAVAILABLE BIT(4)

#define RAWHID_APP_COMBO_WIRE_ITEM_LEN 52

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
#define RAWHID_APP_COMBO_SETTINGS_SUBTREE "keylink/cmb/v1"
#define RAWHID_APP_COMBO_SETTINGS_TABLE_NAME "table"
#define RAWHID_APP_COMBO_SETTINGS_HEADER_LEN 16
#define RAWHID_APP_COMBO_SETTINGS_SLOT_LEN 64
#define RAWHID_APP_COMBO_SETTINGS_TABLE_LEN 2064
#define RAWHID_APP_COMBO_SETTINGS_SLOT_COUNT 32
#define RAWHID_APP_COMBO_SETTINGS_HEADER_CRC_OFFSET 12
#define RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_OFFSET 60
#define RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN 60
#define RAWHID_APP_COMBO_SETTINGS_IDENTITY_OFFSET 52
#define RAWHID_APP_COMBO_SETTINGS_IDENTITY_LEN 8

BUILD_ASSERT(RAWHID_APP_COMBO_SETTINGS_HEADER_LEN == 16, "Combo Settings header must be 16 bytes");
BUILD_ASSERT(RAWHID_APP_COMBO_SETTINGS_SLOT_LEN == 64, "Combo Settings slot must be 64 bytes");
BUILD_ASSERT(RAWHID_APP_COMBO_SETTINGS_SLOT_COUNT == RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS,
             "Combo Settings slot count must match runtime");
BUILD_ASSERT(RAWHID_APP_COMBO_SETTINGS_TABLE_LEN ==
                 RAWHID_APP_COMBO_SETTINGS_HEADER_LEN +
                     RAWHID_APP_COMBO_SETTINGS_SLOT_LEN * RAWHID_APP_COMBO_SETTINGS_SLOT_COUNT,
             "Combo Settings table length mismatch");
BUILD_ASSERT(RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_OFFSET + sizeof(uint32_t) ==
                 RAWHID_APP_COMBO_SETTINGS_SLOT_LEN,
             "Combo Settings slot CRC offset mismatch");
BUILD_ASSERT(RAWHID_APP_COMBO_WIRE_ITEM_LEN == 52, "Combo wire item must be 52 bytes");
#if !(IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST) && IS_ENABLED(CONFIG_BOARD_NATIVE_SIM))
BUILD_ASSERT(DT_NODE_EXISTS(DT_NODELABEL(storage_partition)),
             "Combo Settings v1 requires a storage_partition NVS backend");
BUILD_ASSERT(IS_ENABLED(CONFIG_SETTINGS_NVS),
             "Combo Settings v1 requires the Zephyr Settings NVS backend");
BUILD_ASSERT(DT_PROP(DT_PARENT(DT_PARENT(DT_NODELABEL(storage_partition))), erase_block_size) *
                     CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT >=
                 RAWHID_APP_COMBO_SETTINGS_TABLE_LEN + 64,
             "Combo Settings table does not fit in NVS erase block with 64 byte margin");
#endif
#endif

#if !DT_NODE_EXISTS(DT_PATH(combos))
#error "Keylink Combo runtime requires /combos"
#endif

#define RAWHID_APP_COMBO_COUNT_CHILD(node_id) +1
#define RAWHID_APP_COMBO_CHILD_COUNT (0 DT_FOREACH_CHILD(DT_PATH(combos), RAWHID_APP_COMBO_COUNT_CHILD))

BUILD_ASSERT(RAWHID_APP_COMBO_CHILD_COUNT <= RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS,
             "Keylink Combo: /combos has more than 32 children");

struct rawhid_app_combo_default {
    const char *node_name;
    struct rawhid_app_combo_runtime_definition definition;
};

struct rawhid_app_combo_slot {
    bool occupied;
    uint8_t source_order;
    struct rawhid_app_combo_runtime_definition definition;
};

struct rawhid_app_active_combo {
    uint8_t slot;
    uint8_t key_positions_pressed_count;
    struct zmk_position_state_changed_event
        key_positions_pressed[RAWHID_APP_COMBO_RUNTIME_MAX_KEYS];
};

#define RAWHID_APP_COMBO_PROP_BIT_AT_IDX(node_id, prop, idx) BIT(DT_PROP_BY_IDX(node_id, prop, idx))
#define RAWHID_APP_COMBO_NODE_PROP_BITMASK(node_id, prop)                                         \
    COND_CODE_1(DT_NODE_HAS_PROP(node_id, prop),                                                   \
                (DT_FOREACH_PROP_ELEM_SEP(node_id, prop, RAWHID_APP_COMBO_PROP_BIT_AT_IDX, (|))), \
                (0))

#define RAWHID_APP_COMBO_ASSERT_POSITION(node_id, prop, idx)                                      \
    BUILD_ASSERT(DT_PROP_BY_IDX(node_id, prop, idx) < ZMK_KEYMAP_LEN,                              \
                 "Keylink Combo: key-position outside ZMK_KEYMAP_LEN");

#define RAWHID_APP_COMBO_ASSERT_LAYER(node_id, prop, idx)                                         \
    BUILD_ASSERT(DT_PROP_BY_IDX(node_id, prop, idx) < 32,                                          \
                 "Keylink Combo: layer index cannot be represented by layer mask");

#define RAWHID_APP_COMBO_ASSERT_LAYERS(node_id)                                                    \
    COND_CODE_1(DT_NODE_HAS_PROP(node_id, layers),                                                 \
                (DT_FOREACH_PROP_ELEM(node_id, layers, RAWHID_APP_COMBO_ASSERT_LAYER)), ())

#define RAWHID_APP_COMBO_VALIDATE_NODE(node_id)                                                    \
    BUILD_ASSERT(DT_PROP_LEN(node_id, key_positions) >= 2 &&                                       \
                     DT_PROP_LEN(node_id, key_positions) <= RAWHID_APP_COMBO_RUNTIME_MAX_KEYS,     \
                 "Keylink Combo: key-positions length must be 2..8");                            \
    BUILD_ASSERT(DT_PROP_LEN(node_id, bindings) == 1,                                               \
                 "Keylink Combo: bindings must contain exactly one binding");                    \
    BUILD_ASSERT(DT_PROP(node_id, timeout_ms) >= 1 && DT_PROP(node_id, timeout_ms) <= 1000,        \
                 "Keylink Combo: timeout-ms must be 1..1000");                                  \
    BUILD_ASSERT(DT_PROP(node_id, require_prior_idle_ms) == -1 ||                                 \
                     (DT_PROP(node_id, require_prior_idle_ms) >= 1 &&                             \
                      DT_PROP(node_id, require_prior_idle_ms) <= 1000),                           \
                 "Keylink Combo: require-prior-idle-ms must be omitted or 1..1000");             \
    DT_FOREACH_PROP_ELEM(node_id, key_positions, RAWHID_APP_COMBO_ASSERT_POSITION);                \
    RAWHID_APP_COMBO_ASSERT_LAYERS(node_id)

DT_FOREACH_CHILD(DT_PATH(combos), RAWHID_APP_COMBO_VALIDATE_NODE);

#define RAWHID_APP_COMBO_DEFAULT_ENTRY(node_id)                                                    \
    {                                                                                              \
        .node_name = DT_NODE_FULL_NAME(node_id),                                                   \
        .definition = {.key_count = DT_PROP_LEN(node_id, key_positions),                           \
                       .key_positions = DT_PROP(node_id, key_positions),                           \
                       .binding = ZMK_KEYMAP_EXTRACT_BINDING(0, node_id),                          \
                       .layer_mask = RAWHID_APP_COMBO_NODE_PROP_BITMASK(node_id, layers),          \
                       .timeout_ms = DT_PROP(node_id, timeout_ms),                                \
                       .require_prior_idle_ms = DT_PROP(node_id, require_prior_idle_ms) < 0         \
                                                    ? 0                                               \
                                                    : DT_PROP(node_id, require_prior_idle_ms),        \
                       .slow_release = DT_PROP(node_id, slow_release)},                           \
    },

static const struct rawhid_app_combo_default default_combos[] = {
    DT_FOREACH_CHILD(DT_PATH(combos), RAWHID_APP_COMBO_DEFAULT_ENTRY)};

static struct rawhid_app_combo_slot runtime_slots[RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS];
static uint8_t evaluation_order[RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS];
static uint8_t runtime_slot_count;
static struct rawhid_app_combo_runtime_diagnostics combo_diagnostics;
static uint32_t combo_dirty_slots;
static bool combo_metadata_repair_pending;
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
/* A failed single-image write may have committed either complete image.  Until
 * a later direct Settings read establishes which one, the scratch is not a
 * trustworthy saved baseline. */
static bool combo_settings_baseline_refresh_required;
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
/* The sole persisted-image buffer; it also serves stale GET_COMBO reads. */
static uint8_t combo_settings_table[RAWHID_APP_COMBO_SETTINGS_TABLE_LEN] __aligned(4);
#elif IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
/* Native fixtures borrow their caller-owned image; no production buffer is linked. */
static uint8_t *combo_settings_table;
static uint8_t *combo_settings_test_scratch;
static size_t combo_settings_test_scratch_length;
static uint8_t *combo_settings_test_persisted;
static size_t combo_settings_test_persisted_length;
static bool combo_settings_test_persisted_present;
static bool combo_settings_test_storage_read_error;
static int combo_settings_test_write_result;
static bool combo_settings_test_commit_on_write_error;
static unsigned int combo_settings_test_writes;
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS) || IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
static bool combo_settings_record_seen;
static bool combo_settings_read_error;
static bool combo_settings_length_invalid;
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
static const uint8_t *combo_settings_slot(uint8_t slot);
#endif

static uint8_t pressed_keys_count;
static struct zmk_position_state_changed_event
    pressed_keys[RAWHID_APP_COMBO_RUNTIME_MAX_KEYS];
static bool physical_keys_pressed[ZMK_KEYMAP_LEN];
static uint16_t physical_keys_pressed_count;
static uint32_t candidates;
static int16_t fully_pressed_slot = INT16_MAX;
static uint32_t combo_lookup[ZMK_KEYMAP_LEN];
static struct rawhid_app_active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS];
static uint8_t active_combo_count;
static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;
static int64_t last_tapped_timestamp = INT32_MIN;
static int64_t last_combo_timestamp = INT32_MIN;

static void combo_name_normalize(const char *node_name, uint8_t ordinal, char output[16]);
static void combo_normalize_positions(struct rawhid_app_combo_runtime_definition *definition);
static int combo_cleanup(void);

static const struct rawhid_app_combo_slot *combo_slot(uint8_t slot) {
    return slot < ARRAY_SIZE(runtime_slots) && runtime_slots[slot].occupied ? &runtime_slots[slot]
                                                                            : NULL;
}

static bool combo_name_equal_ci(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char a = *left >= 'A' && *left <= 'Z' ? *left + ('a' - 'A') : *left;
        char b = *right >= 'A' && *right <= 'Z' ? *right + ('a' - 'A') : *right;
        if (a != b) {
            return false;
        }
    }
    return *left == *right;
}

static bool combo_name_in_use(const char name[16]) {
    for (size_t i = 0; i < ARRAY_SIZE(runtime_slots); i++) {
        if (runtime_slots[i].occupied && combo_name_equal_ci(runtime_slots[i].definition.name, name)) {
            return true;
        }
    }
    return false;
}

static bool combo_name_is_canonical(const char name[16]) {
    size_t len = 0;
    while (len < RAWHID_APP_COMBO_RUNTIME_NAME_LEN && name[len] != '\0') {
        char c = name[len];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == ',' || c == '.' || c == '_' || c == '+' ||
                       c == '-';
        if (!allowed) {
            return false;
        }
        len++;
    }
    if (len == 0 || len == RAWHID_APP_COMBO_RUNTIME_NAME_LEN) {
        return false;
    }
    for (size_t index = len + 1; index < RAWHID_APP_COMBO_RUNTIME_NAME_LEN; index++) {
        if (name[index] != '\0') {
            return false;
        }
    }
    return true;
}

static void combo_name_normalize(const char *node_name, uint8_t ordinal, char output[16]) {
    const char *name = strrchr(node_name, '/');
    name = name == NULL ? node_name : name + 1;
    size_t written = 0;
    for (; *name != '\0' && written < RAWHID_APP_COMBO_RUNTIME_NAME_LEN - 1; name++) {
        char c = *name;
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == ',' || c == '.' || c == '_' || c == '+' ||
                       c == '-';
        if (allowed) {
            output[written++] = c;
        }
    }
    output[written] = '\0';
    if (written == 0) {
        snprintk(output, RAWHID_APP_COMBO_RUNTIME_NAME_LEN, "combo%u", ordinal + 1);
    }

    if (!combo_name_in_use(output)) {
        return;
    }

    char base[16];
    strncpy(base, output, sizeof(base));
    base[sizeof(base) - 1] = '\0';
    for (uint8_t suffix = 2; suffix < UINT8_MAX; suffix++) {
        char suffix_text[5];
        snprintk(suffix_text, sizeof(suffix_text), "_%u", suffix);
        size_t suffix_len = strlen(suffix_text);
        size_t base_len = MIN(strlen(base), RAWHID_APP_COMBO_RUNTIME_NAME_LEN - 1 - suffix_len);
        memcpy(output, base, base_len);
        memcpy(output + base_len, suffix_text, suffix_len + 1);
        if (!combo_name_in_use(output)) {
            return;
        }
    }
}

static void combo_normalize_positions(struct rawhid_app_combo_runtime_definition *definition) {
    for (uint8_t i = 1; i < definition->key_count; i++) {
        uint16_t value = definition->key_positions[i];
        uint8_t cursor = i;
        while (cursor > 0 && definition->key_positions[cursor - 1] > value) {
            definition->key_positions[cursor] = definition->key_positions[cursor - 1];
            cursor--;
        }
        definition->key_positions[cursor] = value;
    }
}

static bool combo_same_keys(const struct rawhid_app_combo_runtime_definition *left,
                            const struct rawhid_app_combo_runtime_definition *right) {
    return left->key_count == right->key_count &&
           memcmp(left->key_positions, right->key_positions,
                  left->key_count * sizeof(left->key_positions[0])) == 0;
}

static bool combo_layer_sets_overlap(uint32_t left, uint32_t right) {
    return left == 0 || right == 0 || (left & right) != 0;
}

static bool combo_definition_equal(const struct rawhid_app_combo_runtime_definition *left,
                                   const struct rawhid_app_combo_runtime_definition *right) {
    return memcmp(left->name, right->name, sizeof(left->name)) == 0 &&
           left->key_count == right->key_count &&
           memcmp(left->key_positions, right->key_positions, sizeof(left->key_positions)) == 0 &&
           left->binding.behavior_dev == right->binding.behavior_dev &&
           left->binding.param1 == right->binding.param1 &&
           left->binding.param2 == right->binding.param2 && left->layer_mask == right->layer_mask &&
           left->timeout_ms == right->timeout_ms &&
           left->require_prior_idle_ms == right->require_prior_idle_ms &&
           left->slow_release == right->slow_release;
}

static bool combo_binding_valid(const struct zmk_behavior_binding *binding) {
    if (binding->behavior_dev == NULL || zmk_behavior_get_binding(binding->behavior_dev) == NULL) {
        return false;
    }
    int rc = zmk_behavior_validate_binding(binding);
    return rc >= 0 || rc == -ENODEV;
}

int rawhid_app_combo_runtime_validate(const struct rawhid_app_combo_runtime_definition *definition,
                                      int replacing_slot) {
    if (definition == NULL || definition->key_count < 2 ||
        definition->key_count > RAWHID_APP_COMBO_RUNTIME_MAX_KEYS ||
        !combo_binding_valid(&definition->binding) ||
        definition->timeout_ms < 1 || definition->timeout_ms > 1000 ||
        definition->require_prior_idle_ms > 1000 || !combo_name_is_canonical(definition->name)) {
        return -EINVAL;
    }

    struct rawhid_app_combo_runtime_definition normalized = *definition;
    combo_normalize_positions(&normalized);
    for (uint8_t i = 0; i < normalized.key_count; i++) {
        if (normalized.key_positions[i] >= ZMK_KEYMAP_LEN ||
            (i > 0 && normalized.key_positions[i - 1] == normalized.key_positions[i])) {
            return -EINVAL;
        }
    }
    for (uint8_t slot = 0; slot < ARRAY_SIZE(runtime_slots); slot++) {
        if (!runtime_slots[slot].occupied || slot == replacing_slot) {
            continue;
        }
        if (combo_same_keys(&normalized, &runtime_slots[slot].definition) &&
            combo_layer_sets_overlap(normalized.layer_mask, runtime_slots[slot].definition.layer_mask)) {
            return -EEXIST;
        }
        if (combo_name_equal_ci(normalized.name, runtime_slots[slot].definition.name)) {
            return -EEXIST;
        }
    }
    return 0;
}

static void combo_runtime_clear_definitions(void) {
    memset(runtime_slots, 0, sizeof(runtime_slots));
    memset(evaluation_order, 0, sizeof(evaluation_order));
    memset(combo_lookup, 0, sizeof(combo_lookup));
    runtime_slot_count = 0;
}

static int combo_runtime_install_definition(uint8_t slot_index,
                                            const struct rawhid_app_combo_runtime_definition *definition) {
    if (slot_index >= ARRAY_SIZE(runtime_slots) || definition == NULL) {
        return -EINVAL;
    }
    struct rawhid_app_combo_slot *slot = &runtime_slots[slot_index];
    slot->occupied = false;
    slot->source_order = slot_index;
    slot->definition = *definition;
    combo_normalize_positions(&slot->definition);
    int validation = rawhid_app_combo_runtime_validate(&slot->definition, slot_index);
    if (validation != 0) {
        memset(slot, 0, sizeof(*slot));
        return validation;
    }
    slot->occupied = true;
    return 0;
}

static void combo_rebuild_evaluation_order(void) {
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < ARRAY_SIZE(runtime_slots); slot++) {
        if (!runtime_slots[slot].occupied) {
            continue;
        }
        uint8_t index = count++;
        while (index > 0) {
            uint8_t previous_slot = evaluation_order[index - 1];
            const struct rawhid_app_combo_slot *previous = &runtime_slots[previous_slot];
            const struct rawhid_app_combo_slot *current = &runtime_slots[slot];
            if (previous->definition.key_count < current->definition.key_count ||
                (previous->definition.key_count == current->definition.key_count &&
                 previous->source_order <= current->source_order)) {
                break;
            }
            evaluation_order[index] = previous_slot;
            index--;
        }
        evaluation_order[index] = slot;
    }
    runtime_slot_count = count;
}

static void combo_initialize_slot(uint8_t slot) {
    const struct rawhid_app_combo_slot *combo = combo_slot(slot);
    __ASSERT(combo != NULL, "Initializing an empty Combo slot");
    for (uint8_t key = 0; key < combo->definition.key_count; key++) {
        combo_lookup[combo->definition.key_positions[key]] |= BIT(slot);
    }
}

static void combo_runtime_finalize_definitions(void) {
    combo_rebuild_evaluation_order();
    for (uint8_t slot = 0; slot < ARRAY_SIZE(runtime_slots); slot++) {
        if (runtime_slots[slot].occupied) {
            combo_initialize_slot(slot);
        }
    }
}

static void combo_runtime_rebuild_indexes(void) {
    memset(evaluation_order, 0, sizeof(evaluation_order));
    memset(combo_lookup, 0, sizeof(combo_lookup));
    combo_runtime_finalize_definitions();
}

static int combo_runtime_build_defaults(void) {
    combo_runtime_clear_definitions();
    for (uint8_t index = 0; index < ARRAY_SIZE(default_combos); index++) {
        struct rawhid_app_combo_runtime_definition definition = default_combos[index].definition;
        combo_name_normalize(default_combos[index].node_name, index, definition.name);
        int validation = combo_runtime_install_definition(index, &definition);
        if (validation != 0) {
            LOG_ERR("Keylink Combo default validation failed slot=%u rc=%d", index, validation);
            return validation;
        }
    }
    combo_runtime_rebuild_indexes();
    return 0;
}

static bool combo_active_on_layer(const struct rawhid_app_combo_slot *combo, uint8_t layer) {
    return combo->definition.layer_mask == 0 || (combo->definition.layer_mask & BIT(layer));
}

static bool combo_is_quick_tap(const struct rawhid_app_combo_slot *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->definition.require_prior_idle_ms) > timestamp;
}

static void combo_store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static int combo_setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int candidate_count = 0;
    uint8_t active_layer = zmk_keymap_highest_layer_active();
    uint32_t lookup = combo_lookup[position];
    for (uint8_t order = 0; order < runtime_slot_count; order++) {
        uint8_t slot = evaluation_order[order];
        const struct rawhid_app_combo_slot *combo = combo_slot(slot);
        if ((lookup & BIT(slot)) && combo_active_on_layer(combo, active_layer) &&
            !combo_is_quick_tap(combo, timestamp)) {
            candidates |= BIT(slot);
            candidate_count++;
        }
    }
    return candidate_count;
}

static int combo_filter_candidates(int32_t position) {
    candidates &= combo_lookup[position];
    return candidates == 0 ? 0 : __builtin_popcount(candidates);
}

static int64_t combo_first_candidate_timeout(void) {
    if (pressed_keys_count == 0) {
        return LLONG_MAX;
    }
    int64_t timeout = LLONG_MAX;
    for (uint8_t order = 0; order < runtime_slot_count; order++) {
        uint8_t slot = evaluation_order[order];
        if (candidates & BIT(slot)) {
            timeout = MIN(timeout, combo_slot(slot)->definition.timeout_ms);
        }
    }
    return pressed_keys[0].data.timestamp + timeout;
}

static bool combo_candidate_completely_pressed(const struct rawhid_app_combo_slot *candidate) {
    return candidate->definition.key_count == pressed_keys_count;
}

static int combo_filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Combo timeout without captured keys");
    uint32_t remaining = candidates;
    for (uint8_t order = 0; order < runtime_slot_count; order++) {
        uint8_t slot = evaluation_order[order];
        if ((remaining & BIT(slot)) &&
            pressed_keys[0].data.timestamp + combo_slot(slot)->definition.timeout_ms <= timestamp) {
            candidates &= ~BIT(slot);
        }
    }
    return __builtin_popcount(candidates);
}

static int combo_capture_pressed_key(const struct zmk_position_state_changed *event) {
    if (pressed_keys_count == RAWHID_APP_COMBO_RUNTIME_MAX_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(event);
    return ZMK_EV_EVENT_CAPTURED;
}

static int combo_release_pressed_keys(void) {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (uint8_t index = 0; index < count; index++) {
        if (index == 0) {
            ZMK_EVENT_RELEASE(pressed_keys[index]);
        } else {
            ZMK_EVENT_RAISE(pressed_keys[index]);
        }
    }
    return count;
}

static int combo_invoke_behavior(uint8_t slot, int32_t timestamp, bool pressed) {
    const struct rawhid_app_combo_slot *combo = combo_slot(slot);
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(slot),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    if (pressed) {
        last_combo_timestamp = timestamp;
    }
    return zmk_behavior_invoke_binding(&combo->definition.binding, event, pressed);
}

static void combo_move_pressed_keys_to_active(struct rawhid_app_active_combo *active) {
    uint8_t length = MIN(pressed_keys_count, combo_slot(active->slot)->definition.key_count);
    for (uint8_t index = 0; index < length; index++) {
        active->key_positions_pressed[index] = pressed_keys[index];
    }
    active->key_positions_pressed_count = length;
    for (uint8_t index = 0; index + length < pressed_keys_count; index++) {
        pressed_keys[index] = pressed_keys[index + length];
    }
    pressed_keys_count -= length;
}

static struct rawhid_app_active_combo *combo_store_active(uint8_t slot) {
    for (uint8_t index = 0; index < ARRAY_SIZE(active_combos); index++) {
        if (active_combos[index].slot == UINT8_MAX) {
            active_combos[index].slot = slot;
            active_combo_count++;
            return &active_combos[index];
        }
    }
    LOG_ERR("Keylink Combo active limit reached (%d)", CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void combo_activate(uint8_t slot) {
    struct rawhid_app_active_combo *active = combo_store_active(slot);
    if (active == NULL) {
        combo_release_pressed_keys();
        return;
    }
    combo_move_pressed_keys_to_active(active);
    combo_invoke_behavior(slot, active->key_positions_pressed[0].data.timestamp, true);
}

static void combo_deactivate(uint8_t index) {
    active_combo_count--;
    if (index != active_combo_count) {
        active_combos[index] = active_combos[active_combo_count];
    }
    active_combos[active_combo_count] = (struct rawhid_app_active_combo){.slot = UINT8_MAX};
}

static bool combo_release_key(int32_t position, int64_t timestamp) {
    for (uint8_t combo_index = 0; combo_index < active_combo_count; combo_index++) {
        struct rawhid_app_active_combo *active = &active_combos[combo_index];
        bool key_released = false;
        bool all_keys_pressed = active->key_positions_pressed_count ==
                                combo_slot(active->slot)->definition.key_count;
        bool all_keys_released = true;
        for (uint8_t index = 0; index < active->key_positions_pressed_count; index++) {
            if (key_released) {
                active->key_positions_pressed[index - 1] = active->key_positions_pressed[index];
                all_keys_released = false;
            } else if (active->key_positions_pressed[index].data.position != position) {
                all_keys_released = false;
            } else {
                key_released = true;
            }
        }
        if (!key_released) {
            continue;
        }
        active->key_positions_pressed_count--;
        const struct rawhid_app_combo_runtime_definition *definition =
            &combo_slot(active->slot)->definition;
        if ((definition->slow_release && all_keys_released) ||
            (!definition->slow_release && all_keys_pressed)) {
            combo_invoke_behavior(active->slot, timestamp, false);
        }
        if (all_keys_released) {
            combo_deactivate(combo_index);
        }
        return true;
    }
    return false;
}

static int combo_cleanup(void) {
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    candidates = 0;
    if (fully_pressed_slot != INT16_MAX) {
        combo_activate(fully_pressed_slot);
        fully_pressed_slot = INT16_MAX;
    }
    return combo_release_pressed_keys();
}

static void combo_update_timeout_task(void) {
    int64_t timeout = combo_first_candidate_timeout();
    if (timeout_task_timeout_at == timeout) {
        return;
    }
    if (timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(MAX(timeout - k_uptime_get(), 0))) >= 0) {
        timeout_task_timeout_at = timeout;
    }
}

static int combo_position_down(struct zmk_position_state_changed *event) {
    int candidate_count;
    if (pressed_keys_count == 0) {
        candidate_count = combo_setup_candidates_for_first_keypress(event->position, event->timestamp);
        if (candidate_count == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        combo_filter_timed_out_candidates(event->timestamp);
        candidate_count = combo_filter_candidates(event->position);
    }
    int result = combo_capture_pressed_key(event);
    combo_update_timeout_task();
    if (candidate_count != 0) {
        for (uint8_t order = 0; order < runtime_slot_count; order++) {
            uint8_t slot = evaluation_order[order];
            if (!(candidates & BIT(slot))) {
                continue;
            }
            if (combo_candidate_completely_pressed(combo_slot(slot))) {
                fully_pressed_slot = slot;
                if (candidate_count == 1) {
                    combo_cleanup();
                }
            }
            return result;
        }
    }
    combo_cleanup();
    return result;
}

static int combo_position_up(struct zmk_position_state_changed *event) {
    int released_keys = combo_cleanup();
    if (combo_release_key(event->position, event->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        struct zmk_position_state_changed_event duplicate = copy_raised_zmk_position_state_changed(event);
        ZMK_EVENT_RAISE(duplicate);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        return;
    }
    if (combo_filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        combo_cleanup();
    }
    combo_update_timeout_task();
}

static int combo_listener(const zmk_event_t *event_header) {
    struct zmk_position_state_changed *event = as_zmk_position_state_changed(event_header);
    if (event != NULL) {
        if (event->position >= 0 && event->position < ZMK_KEYMAP_LEN) {
            if (event->state && !physical_keys_pressed[event->position]) {
                physical_keys_pressed[event->position] = true;
                physical_keys_pressed_count++;
            } else if (!event->state && physical_keys_pressed[event->position]) {
                physical_keys_pressed[event->position] = false;
                physical_keys_pressed_count--;
            }
        }
        return event->state ? combo_position_down(event) : combo_position_up(event);
    }
    struct zmk_keycode_state_changed *keycode = as_zmk_keycode_state_changed(event_header);
    if (keycode != NULL && keycode->state && !is_mod(keycode->usage_page, keycode->keycode)) {
        combo_store_last_tapped(keycode->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

void rawhid_app_combo_runtime_get_limits(struct rawhid_app_combo_runtime_limits *limits) {
    if (limits != NULL) {
        *limits = (struct rawhid_app_combo_runtime_limits){
            .max_slots = RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS,
            .max_keys = RAWHID_APP_COMBO_RUNTIME_MAX_KEYS,
            .occupied_count = runtime_slot_count,
        };
    }
}

uint32_t rawhid_app_combo_runtime_occupied_mask(void) {
    uint32_t mask = 0;
    for (uint8_t slot = 0; slot < ARRAY_SIZE(runtime_slots); slot++) {
        if (runtime_slots[slot].occupied) {
            mask |= BIT(slot);
        }
    }
    return mask;
}

bool rawhid_app_combo_runtime_get(uint8_t slot,
                                  struct rawhid_app_combo_runtime_definition *definition) {
    const struct rawhid_app_combo_slot *runtime = combo_slot(slot);
    if (runtime == NULL || definition == NULL) {
        return false;
    }
    *definition = runtime->definition;
    return true;
}

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
static bool combo_runtime_matches_saved_slot(uint8_t slot) {
    if ((combo_diagnostics.flags & RAWHID_APP_COMBO_SETTINGS_FLAGS_SAVED_TABLE_LOADED) == 0 ||
        (combo_diagnostics.stale_slots | combo_diagnostics.invalid_slots) & BIT(slot)) {
        return false;
    }

    const uint8_t *record = combo_settings_slot(slot);
    bool saved_occupied = (sys_get_le32(&combo_settings_table[8]) & BIT(slot)) != 0;
    const struct rawhid_app_combo_slot *runtime = combo_slot(slot);
    if (!saved_occupied) {
        return runtime == NULL;
    }
    if (runtime == NULL || (record[0] & BIT(0)) == 0) {
        return false;
    }

    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(runtime->definition.binding.behavior_dev);
    if (behavior_id == UINT16_MAX ||
        record[1] !=
            (runtime->definition.key_count | (runtime->definition.slow_release ? BIT(4) : 0)) ||
        memcmp(&record[2], runtime->definition.name, sizeof(runtime->definition.name)) != 0 ||
        sys_get_le16(&record[34]) != behavior_id ||
        sys_get_le32(&record[36]) != runtime->definition.binding.param1 ||
        sys_get_le32(&record[40]) != runtime->definition.binding.param2 ||
        sys_get_le32(&record[44]) != runtime->definition.layer_mask ||
        sys_get_le16(&record[48]) != runtime->definition.timeout_ms ||
        sys_get_le16(&record[50]) !=
            (runtime->definition.require_prior_idle_ms == 0 ? UINT16_MAX
                                                            : runtime->definition.require_prior_idle_ms)) {
        return false;
    }
    /* Default-combo definitions do not promise a canonical value in their
     * unused position tail.  The persisted record does, so compare only the
     * semantically occupied keys after the decoder has checked that tail. */
    for (uint8_t index = 0; index < runtime->definition.key_count; index++) {
        if (sys_get_le16(&record[18 + index * sizeof(uint16_t)]) !=
            runtime->definition.key_positions[index]) {
            return false;
        }
    }
    return true;
}
#endif

static void combo_runtime_update_dirty_slot(uint8_t slot) {
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
    if (combo_runtime_matches_saved_slot(slot)) {
        combo_dirty_slots &= ~BIT(slot);
    } else {
        combo_dirty_slots |= BIT(slot);
    }
#else
    ARG_UNUSED(slot);
#endif
}

static void combo_runtime_note_mutation(void) {
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
    if ((combo_diagnostics.flags & (RAWHID_APP_COMBO_SETTINGS_FLAGS_METADATA_INVALID_FALLBACK |
                                    RAWHID_APP_COMBO_SETTINGS_FLAGS_VERSION_UNSUPPORTED_FALLBACK |
                                    RAWHID_APP_COMBO_SETTINGS_FLAGS_READ_ERROR_FALLBACK)) != 0) {
        combo_metadata_repair_pending = true;
    }
#endif
}

int rawhid_app_combo_runtime_upsert(uint8_t slot,
                                    const struct rawhid_app_combo_runtime_definition *definition) {
    if (slot >= ARRAY_SIZE(runtime_slots) || definition == NULL) {
        return -EINVAL;
    }

    struct rawhid_app_combo_runtime_definition normalized = *definition;
    combo_normalize_positions(&normalized);
    int rc = rawhid_app_combo_runtime_validate(&normalized, slot);
    if (rc != 0) {
        return rc;
    }

    if (runtime_slots[slot].occupied && combo_definition_equal(&runtime_slots[slot].definition, &normalized)) {
        return 0;
    }

    runtime_slots[slot] = (struct rawhid_app_combo_slot){
        .occupied = true,
        .source_order = slot,
        .definition = normalized,
    };
    combo_runtime_rebuild_indexes();
    combo_runtime_update_dirty_slot(slot);
    combo_runtime_note_mutation();
    return 0;
}

int rawhid_app_combo_runtime_delete(uint8_t slot) {
    if (slot >= ARRAY_SIZE(runtime_slots)) {
        return -EINVAL;
    }
    bool saved_diagnostic = (combo_diagnostics.stale_slots | combo_diagnostics.invalid_slots) & BIT(slot);
    if (!runtime_slots[slot].occupied && !saved_diagnostic) {
        return 0;
    }
    memset(&runtime_slots[slot], 0, sizeof(runtime_slots[slot]));
    combo_runtime_rebuild_indexes();
    combo_runtime_update_dirty_slot(slot);
    combo_runtime_note_mutation();
    return 0;
}

bool rawhid_app_combo_runtime_dirty(void) {
    return combo_dirty_slots != 0 || combo_metadata_repair_pending;
}

int rawhid_app_combo_runtime_reset_to_keymap(void) {
    int rc = combo_runtime_build_defaults();
    if (rc != 0) {
        return rc;
    }

    combo_dirty_slots = 0;
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
    if (combo_diagnostics.flags & RAWHID_APP_COMBO_SETTINGS_FLAGS_SAVED_TABLE_LOADED) {
        for (uint8_t slot = 0; slot < RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS; slot++) {
            combo_runtime_update_dirty_slot(slot);
        }
    } else if (combo_diagnostics.flags &
               (RAWHID_APP_COMBO_SETTINGS_FLAGS_METADATA_INVALID_FALLBACK |
                RAWHID_APP_COMBO_SETTINGS_FLAGS_VERSION_UNSUPPORTED_FALLBACK |
                RAWHID_APP_COMBO_SETTINGS_FLAGS_READ_ERROR_FALLBACK)) {
        combo_metadata_repair_pending = true;
    }
#endif
    return 0;
}

bool rawhid_app_combo_runtime_keys_pressed(void) { return physical_keys_pressed_count != 0; }

bool rawhid_app_combo_runtime_idle(void) {
    return physical_keys_pressed_count == 0 && pressed_keys_count == 0 && candidates == 0 &&
           fully_pressed_slot == INT16_MAX && active_combo_count == 0 && timeout_task_timeout_at == 0;
}

void rawhid_app_combo_runtime_get_diagnostics(struct rawhid_app_combo_runtime_diagnostics *diagnostics) {
    if (diagnostics == NULL) {
        return;
    }
    *diagnostics = combo_diagnostics;
#if !IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
    diagnostics->flags |= RAWHID_APP_COMBO_SETTINGS_FLAGS_STORAGE_UNAVAILABLE;
#endif
}

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
enum combo_saved_record_state {
    COMBO_SAVED_VALID,
    COMBO_SAVED_STALE,
    COMBO_SAVED_INVALID,
    COMBO_SAVED_TOMBSTONE,
};

static const uint8_t *combo_settings_slot(uint8_t slot) {
    return &combo_settings_table[RAWHID_APP_COMBO_SETTINGS_HEADER_LEN +
                                 slot * RAWHID_APP_COMBO_SETTINGS_SLOT_LEN];
}

static bool combo_settings_header_valid(bool *unsupported_version) {
    *unsupported_version = false;
    if (memcmp(combo_settings_table, "KCMB", 4) != 0) {
        return false;
    }
    if (combo_settings_table[4] != 1) {
        *unsupported_version = true;
        return false;
    }
    if (combo_settings_table[5] != RAWHID_APP_COMBO_SETTINGS_SLOT_LEN ||
        combo_settings_table[6] != RAWHID_APP_COMBO_SETTINGS_SLOT_COUNT ||
        combo_settings_table[7] != 0 ||
        sys_get_le32(&combo_settings_table[RAWHID_APP_COMBO_SETTINGS_HEADER_CRC_OFFSET]) !=
            crc32_ieee(combo_settings_table, RAWHID_APP_COMBO_SETTINGS_HEADER_CRC_OFFSET)) {
        return false;
    }
    return true;
}

static void combo_settings_encode_tombstone(uint8_t slot) {
    uint8_t *record = &combo_settings_table[RAWHID_APP_COMBO_SETTINGS_HEADER_LEN +
                                            slot * RAWHID_APP_COMBO_SETTINGS_SLOT_LEN];
    memset(record, 0, RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN);
    sys_put_le32(crc32_ieee(record, RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN),
                 &record[RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_OFFSET]);
}

static int combo_settings_encode_runtime_slot(uint8_t slot) {
    uint8_t *record = &combo_settings_table[RAWHID_APP_COMBO_SETTINGS_HEADER_LEN +
                                            slot * RAWHID_APP_COMBO_SETTINGS_SLOT_LEN];
    const struct rawhid_app_combo_slot *runtime = combo_slot(slot);
    if (runtime == NULL) {
        combo_settings_encode_tombstone(slot);
        return 0;
    }
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(runtime->definition.binding.behavior_dev);
    if (behavior_id == UINT16_MAX) {
        return -EINVAL;
    }
    memset(record, 0, RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN);
    record[0] = BIT(0);
    record[1] = runtime->definition.key_count | (runtime->definition.slow_release ? BIT(4) : 0);
    memcpy(&record[2], runtime->definition.name, sizeof(runtime->definition.name));
    for (uint8_t key = 0; key < RAWHID_APP_COMBO_RUNTIME_MAX_KEYS; key++) {
        sys_put_le16(key < runtime->definition.key_count ? runtime->definition.key_positions[key]
                                                          : UINT16_MAX,
                     &record[18 + key * sizeof(uint16_t)]);
    }
    sys_put_le16(behavior_id, &record[34]);
    sys_put_le32(runtime->definition.binding.param1, &record[36]);
    sys_put_le32(runtime->definition.binding.param2, &record[40]);
    sys_put_le32(runtime->definition.layer_mask, &record[44]);
    sys_put_le16(runtime->definition.timeout_ms, &record[48]);
    sys_put_le16(runtime->definition.require_prior_idle_ms == 0 ? UINT16_MAX
                                                                 : runtime->definition.require_prior_idle_ms,
                 &record[50]);
    rawhid_app_behavior_identity_hash(&runtime->definition.binding,
                                      &record[RAWHID_APP_COMBO_SETTINGS_IDENTITY_OFFSET],
                                      RAWHID_APP_COMBO_SETTINGS_IDENTITY_LEN);
    sys_put_le32(crc32_ieee(record, RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN),
                 &record[RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_OFFSET]);
    return 0;
}

static void combo_settings_encode_header(uint32_t rewritten_slots, bool build_full_table) {
    uint32_t occupied = build_full_table ? rawhid_app_combo_runtime_occupied_mask()
                                         : sys_get_le32(&combo_settings_table[8]);
    if (!build_full_table) {
        for (uint8_t slot = 0; slot < RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS; slot++) {
            if ((rewritten_slots & BIT(slot)) == 0) {
                continue;
            }
            if (combo_slot(slot) != NULL) {
                occupied |= BIT(slot);
            } else {
                occupied &= ~BIT(slot);
            }
        }
    }
    memcpy(combo_settings_table, "KCMB", 4);
    combo_settings_table[4] = 1;
    combo_settings_table[5] = RAWHID_APP_COMBO_SETTINGS_SLOT_LEN;
    combo_settings_table[6] = RAWHID_APP_COMBO_SETTINGS_SLOT_COUNT;
    combo_settings_table[7] = 0;
    sys_put_le32(occupied, &combo_settings_table[8]);
    sys_put_le32(crc32_ieee(combo_settings_table, RAWHID_APP_COMBO_SETTINGS_HEADER_CRC_OFFSET),
                 &combo_settings_table[RAWHID_APP_COMBO_SETTINGS_HEADER_CRC_OFFSET]);
}

static bool combo_settings_binding_from_record(const uint8_t *record,
                                               struct zmk_behavior_binding *binding) {
    zmk_behavior_local_id_t behavior_id = sys_get_le16(&record[34]);
    if (behavior_id == UINT16_MAX) {
        return false;
    }
    const char *behavior_dev = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (behavior_dev == NULL) {
        return false;
    }
    *binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_dev,
        .param1 = sys_get_le32(&record[36]),
        .param2 = sys_get_le32(&record[40]),
    };
    int rc = zmk_behavior_validate_binding(binding);
    return rc >= 0 || (rc == -ENODEV && zmk_behavior_get_binding(behavior_dev) != NULL);
}

static bool combo_settings_record_encoding_valid(const uint8_t *record, uint8_t key_count) {
    if (record[1] & 0xe0) {
        return false;
    }
    /* NUL termination and tail zero are structural; semantic name validity is stale. */
    bool nul_seen = false;
    for (uint8_t index = 0; index < RAWHID_APP_COMBO_RUNTIME_NAME_LEN; index++) {
        uint8_t value = record[2 + index];
        if (nul_seen && value != 0) {
            return false;
        }
        nul_seen |= value == 0;
    }
    if (!nul_seen) {
        return false;
    }
    for (uint8_t index = key_count; index < RAWHID_APP_COMBO_RUNTIME_MAX_KEYS; index++) {
        if (sys_get_le16(&record[18 + index * 2]) != UINT16_MAX) {
            return false;
        }
    }
    return true;
}

static enum combo_saved_record_state
combo_settings_decode_record(uint8_t slot, uint32_t occupied_mask,
                             struct rawhid_app_combo_runtime_definition *definition) {
    const uint8_t *record = combo_settings_slot(slot);
    bool header_occupied = (occupied_mask & BIT(slot)) != 0;
    if ((record[0] & ~BIT(0)) != 0 ||
        sys_get_le32(&record[RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_OFFSET]) !=
            crc32_ieee(record, RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN) ||
        header_occupied != ((record[0] & BIT(0)) != 0)) {
        return COMBO_SAVED_INVALID;
    }
    if (!header_occupied) {
        for (uint8_t index = 1; index < RAWHID_APP_COMBO_SETTINGS_SLOT_CRC_LEN; index++) {
            if (record[index] != 0) {
                return COMBO_SAVED_INVALID;
            }
        }
        return COMBO_SAVED_TOMBSTONE;
    }

    uint8_t key_count = record[1] & 0x0f;
    if (!combo_settings_record_encoding_valid(record, key_count)) {
        return COMBO_SAVED_INVALID;
    }
    memset(definition, 0, sizeof(*definition));
    memcpy(definition->name, &record[2], sizeof(definition->name));
    definition->key_count = key_count;
    for (uint8_t index = 0; index < RAWHID_APP_COMBO_RUNTIME_MAX_KEYS; index++) {
        definition->key_positions[index] = sys_get_le16(&record[18 + index * 2]);
    }
    definition->layer_mask = sys_get_le32(&record[44]);
    definition->timeout_ms = sys_get_le16(&record[48]);
    uint16_t prior_idle = sys_get_le16(&record[50]);
    definition->require_prior_idle_ms = prior_idle == UINT16_MAX ? 0 : prior_idle;
    definition->slow_release = (record[1] & BIT(4)) != 0;

    if (!combo_settings_binding_from_record(record, &definition->binding)) {
        return COMBO_SAVED_STALE;
    }
    uint8_t hash[RAWHID_APP_COMBO_SETTINGS_IDENTITY_LEN];
    rawhid_app_behavior_identity_hash(&definition->binding, hash, sizeof(hash));
    if (memcmp(hash, &record[RAWHID_APP_COMBO_SETTINGS_IDENTITY_OFFSET], sizeof(hash)) != 0 ||
        rawhid_app_combo_runtime_validate(definition, slot) != 0) {
        return COMBO_SAVED_STALE;
    }
    return COMBO_SAVED_VALID;
}

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
static int combo_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, RAWHID_APP_COMBO_SETTINGS_TABLE_NAME) != 0) {
        return 0;
    }
    combo_settings_record_seen = true;
    if (len != RAWHID_APP_COMBO_SETTINGS_TABLE_LEN) {
        combo_settings_length_invalid = true;
    } else if (read_cb(cb_arg, combo_settings_table, sizeof(combo_settings_table)) !=
               (int)sizeof(combo_settings_table)) {
        combo_settings_read_error = true;
    }
    return 0;
}

static int combo_settings_reload_direct(const char *name, size_t len, settings_read_cb read_cb,
                                        void *cb_arg, void *param) {
    ARG_UNUSED(param);
    return combo_settings_set(name, len, read_cb, cb_arg);
}
#endif

static int combo_settings_commit(void) {
    combo_diagnostics = (struct rawhid_app_combo_runtime_diagnostics){0};
    combo_dirty_slots = 0;
    combo_metadata_repair_pending = false;
    combo_settings_baseline_refresh_required = false;
    /* Every production and fixture load starts from the same safe default state. */
    int defaults_rc = combo_runtime_build_defaults();
    if (defaults_rc != 0) {
        return defaults_rc;
    }
    if (!combo_settings_record_seen) {
        return 0; /* First boot: defaults remain, no fallback diagnostic. */
    }
    if (combo_settings_read_error) {
        combo_diagnostics.flags |= RAWHID_APP_COMBO_SETTINGS_FLAGS_READ_ERROR_FALLBACK;
        return 0;
    }

    if (combo_settings_length_invalid) {
        combo_diagnostics.flags |= RAWHID_APP_COMBO_SETTINGS_FLAGS_METADATA_INVALID_FALLBACK;
        return 0;
    }

    bool unsupported_version;
    if (!combo_settings_header_valid(&unsupported_version)) {
        combo_diagnostics.flags |= unsupported_version
                                      ? RAWHID_APP_COMBO_SETTINGS_FLAGS_VERSION_UNSUPPORTED_FALLBACK
                                      : RAWHID_APP_COMBO_SETTINGS_FLAGS_METADATA_INVALID_FALLBACK;
        return 0;
    }

    combo_diagnostics.flags |= RAWHID_APP_COMBO_SETTINGS_FLAGS_SAVED_TABLE_LOADED;
    combo_runtime_clear_definitions();
    uint32_t occupied_mask = sys_get_le32(&combo_settings_table[8]);
    for (uint8_t slot = 0; slot < RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS; slot++) {
        struct rawhid_app_combo_runtime_definition definition;
        enum combo_saved_record_state state =
            combo_settings_decode_record(slot, occupied_mask, &definition);
        switch (state) {
        case COMBO_SAVED_VALID:
            if (combo_runtime_install_definition(slot, &definition) != 0) {
                combo_diagnostics.stale_slots |= BIT(slot);
            }
            break;
        case COMBO_SAVED_STALE:
            combo_diagnostics.stale_slots |= BIT(slot);
            break;
        case COMBO_SAVED_INVALID:
            combo_diagnostics.invalid_slots |= BIT(slot);
            break;
        case COMBO_SAVED_TOMBSTONE:
            break;
        }
    }
    combo_runtime_finalize_definitions();
    return 0;
}

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
void rawhid_app_combo_settings_test_storage(uint8_t *image, size_t length, bool present,
                                            bool read_error, int write_result,
                                            bool commit_on_write_error) {
    combo_settings_test_persisted = image;
    combo_settings_test_persisted_length = length;
    combo_settings_test_persisted_present = present;
    combo_settings_test_storage_read_error = read_error;
    combo_settings_test_write_result = write_result;
    combo_settings_test_commit_on_write_error = commit_on_write_error;
    combo_settings_test_writes = 0;
}

unsigned int rawhid_app_combo_settings_test_write_count(void) {
    return combo_settings_test_writes;
}

static int combo_settings_refresh_baseline(void) {
    if (combo_settings_test_storage_read_error || !combo_settings_test_persisted_present ||
        combo_settings_test_persisted == NULL ||
        combo_settings_test_persisted_length != RAWHID_APP_COMBO_SETTINGS_TABLE_LEN ||
        combo_settings_table == NULL) {
        return -EIO;
    }
    memcpy(combo_settings_table, combo_settings_test_persisted,
           RAWHID_APP_COMBO_SETTINGS_TABLE_LEN);
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
static int combo_settings_refresh_baseline(void) {
    combo_settings_record_seen = false;
    combo_settings_read_error = false;
    combo_settings_length_invalid = false;
    int rc = settings_load_subtree_direct(RAWHID_APP_COMBO_SETTINGS_SUBTREE,
                                          combo_settings_reload_direct, NULL);
    if (rc != 0 || combo_settings_read_error || combo_settings_length_invalid ||
        !combo_settings_record_seen) {
        return -EIO;
    }
    return 0;
}
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
static int combo_settings_test_begin(void) {
    combo_settings_record_seen = false;
    combo_settings_read_error = false;
    combo_settings_length_invalid = false;
    combo_settings_table = combo_settings_test_scratch_length == RAWHID_APP_COMBO_SETTINGS_TABLE_LEN
                               ? combo_settings_test_scratch
                               : NULL;
    return 0;
}

void rawhid_app_combo_settings_test_scratch(uint8_t *image, size_t length) {
    combo_settings_test_scratch = image;
    combo_settings_test_scratch_length = length;
}

int rawhid_app_combo_settings_test_load_image(const uint8_t *image, size_t length) {
    combo_settings_test_begin();
    combo_settings_record_seen = true;
    if (image == NULL) {
        if (length != 0) {
            return -EINVAL;
        }
        combo_settings_length_invalid = true;
    } else if (length != RAWHID_APP_COMBO_SETTINGS_TABLE_LEN) {
        combo_settings_length_invalid = true;
    } else if (combo_settings_table == NULL) {
        combo_settings_length_invalid = true;
    } else if (image != combo_settings_table) {
        memcpy(combo_settings_table, image, RAWHID_APP_COMBO_SETTINGS_TABLE_LEN);
    }
    return combo_settings_commit();
}

int rawhid_app_combo_settings_test_load_missing(void) {
    combo_settings_test_begin();
    return combo_settings_commit();
}

int rawhid_app_combo_settings_test_load_read_error(void) {
    combo_settings_test_begin();
    combo_settings_record_seen = true;
    combo_settings_read_error = true;
    return combo_settings_commit();
}
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
SETTINGS_STATIC_HANDLER_DEFINE_WITH_CPRIO(rawhid_app_combo_runtime,
                                          RAWHID_APP_COMBO_SETTINGS_SUBTREE, NULL,
                                          combo_settings_set, combo_settings_commit, NULL, 11);
#endif
int rawhid_app_combo_runtime_get_stale_item(uint8_t slot, uint8_t item[52]) {
    if (combo_settings_baseline_refresh_required) {
        return -EIO;
    }
    if (item == NULL || slot >= RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS ||
        (combo_diagnostics.stale_slots & BIT(slot)) == 0) {
        return 0;
    }
    const uint8_t *record = combo_settings_slot(slot);
    item[0] = slot;
    memcpy(&item[1], &record[1], RAWHID_APP_COMBO_WIRE_ITEM_LEN - 1);
    return 1;
}

int rawhid_app_combo_runtime_discard(void) {
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
    if (combo_settings_baseline_refresh_required && combo_settings_refresh_baseline() != 0) {
        return -EIO;
    }
#endif
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
    if (combo_settings_baseline_refresh_required) {
        /* The normal Settings handler owns the same single buffer. Reloading
         * through it before commit resolves old-vs-new without a shadow copy. */
        if (combo_settings_refresh_baseline() != 0) {
            return -EIO;
        }
    }
#endif
    return combo_settings_commit();
}

int rawhid_app_combo_runtime_save(void) {
    if (!rawhid_app_combo_runtime_dirty()) {
        return 0;
    }
    /* A read-error fallback has no trustworthy image to merge with.  A later
     * settings reload must establish it before a mutation can be persisted. */
    if (combo_diagnostics.flags & RAWHID_APP_COMBO_SETTINGS_FLAGS_READ_ERROR_FALLBACK) {
        return -EIO;
    }
    bool build_full_table =
        (combo_diagnostics.flags & RAWHID_APP_COMBO_SETTINGS_FLAGS_SAVED_TABLE_LOADED) == 0;
    if (build_full_table) {
        memset(combo_settings_table, 0, RAWHID_APP_COMBO_SETTINGS_TABLE_LEN);
        for (uint8_t slot = 0; slot < RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS; slot++) {
            int rc = combo_settings_encode_runtime_slot(slot);
            if (rc != 0) {
                return rc;
            }
        }
    } else {
        for (uint8_t slot = 0; slot < RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS; slot++) {
            if ((combo_dirty_slots & BIT(slot)) == 0) {
                continue;
            }
            int rc = combo_settings_encode_runtime_slot(slot);
            if (rc != 0) {
                return rc;
            }
        }
    }
    combo_settings_encode_header(combo_dirty_slots, build_full_table);
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS)
    int rc = settings_save_one(RAWHID_APP_COMBO_SETTINGS_SUBTREE "/" RAWHID_APP_COMBO_SETTINGS_TABLE_NAME,
                               combo_settings_table, RAWHID_APP_COMBO_SETTINGS_TABLE_LEN);
    if (rc != 0) {
        combo_settings_baseline_refresh_required = combo_settings_refresh_baseline() != 0;
        return rc;
    }
#elif IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
    combo_settings_test_writes++;
    if (combo_settings_test_persisted != NULL &&
        (combo_settings_test_write_result == 0 || combo_settings_test_commit_on_write_error)) {
        memcpy(combo_settings_test_persisted, combo_settings_table,
               RAWHID_APP_COMBO_SETTINGS_TABLE_LEN);
        combo_settings_test_persisted_length = RAWHID_APP_COMBO_SETTINGS_TABLE_LEN;
        combo_settings_test_persisted_present = true;
    }
    if (combo_settings_test_write_result != 0) {
        combo_settings_baseline_refresh_required = combo_settings_refresh_baseline() != 0;
        return combo_settings_test_write_result;
    }
#endif
    /* Reclassify the exact image just written; it also makes the new image the
     * sole saved baseline and clears dirty only after the synchronous write. */
    combo_settings_record_seen = true;
    combo_settings_read_error = false;
    combo_settings_length_invalid = false;
    return combo_settings_commit();
}
#endif
#if !IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_CORE)
int rawhid_app_combo_runtime_get_stale_item(uint8_t slot, uint8_t item[52]) {
    ARG_UNUSED(slot);
    ARG_UNUSED(item);
    return 0;
}
int rawhid_app_combo_runtime_discard(void) { return -ENOTSUP; }
int rawhid_app_combo_runtime_save(void) { return -ENOTSUP; }
#endif

static int combo_runtime_init(void) {
    for (uint8_t index = 0; index < ARRAY_SIZE(active_combos); index++) {
        active_combos[index].slot = UINT8_MAX;
    }
    k_work_init_delayable(&timeout_task, combo_timeout_handler);

    int rc = combo_runtime_build_defaults();
    if (rc != 0) {
        return rc;
    }
    LOG_INF("Keylink Combo runtime initialized with %u defaults", runtime_slot_count);
    return 0;
}

ZMK_LISTENER(rawhid_app_combo_runtime, combo_listener);
ZMK_SUBSCRIPTION(rawhid_app_combo_runtime, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rawhid_app_combo_runtime, zmk_keycode_state_changed);

SYS_INIT(combo_runtime_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
