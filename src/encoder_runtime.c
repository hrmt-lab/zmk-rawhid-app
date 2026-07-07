#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS MAX(ZMK_KEYMAP_SENSORS_LEN, 1)

enum rawhid_app_encoder_direction {
    RAWHID_APP_ENCODER_DIRECTION_NONE = 0,
    RAWHID_APP_ENCODER_DIRECTION_CW,
    RAWHID_APP_ENCODER_DIRECTION_CCW,
};

struct rawhid_app_encoder_override {
    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    bool valid;
    bool dirty;
};

static struct rawhid_app_encoder_override
    encoder_overrides[ZMK_KEYMAP_LAYERS_LEN][RAWHID_APP_ENCODER_RUNTIME_SENSOR_SLOTS];

bool rawhid_app_encoder_runtime_get(uint32_t layer_id, uint8_t encoder_id,
                                    struct rawhid_app_encoder_runtime_bindings *bindings) {
    if (bindings == NULL || layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return false;
    }

    struct rawhid_app_encoder_override *override = &encoder_overrides[layer_id][encoder_id];
    if (!override->valid) {
        return false;
    }

    bindings->cw_binding = override->cw_binding;
    bindings->ccw_binding = override->ccw_binding;
    bindings->dirty = override->dirty;
    return true;
}

void rawhid_app_encoder_runtime_set(uint32_t layer_id, uint8_t encoder_id,
                                    const struct zmk_behavior_binding *cw_binding,
                                    const struct zmk_behavior_binding *ccw_binding) {
    if (cw_binding == NULL || ccw_binding == NULL || layer_id >= ZMK_KEYMAP_LAYERS_LEN ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        return;
    }

    struct rawhid_app_encoder_override *override = &encoder_overrides[layer_id][encoder_id];
    override->cw_binding = *cw_binding;
    override->ccw_binding = *ccw_binding;
    override->valid = true;
    override->dirty = true;
}

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
encoder_runtime_binding_for_direction(const struct rawhid_app_encoder_override *override,
                                      enum rawhid_app_encoder_direction direction) {
    switch (direction) {
    case RAWHID_APP_ENCODER_DIRECTION_CW:
        return &override->cw_binding;
    case RAWHID_APP_ENCODER_DIRECTION_CCW:
        return &override->ccw_binding;
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

    if (active_layer_id >= ZMK_KEYMAP_LAYERS_LEN) {
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

    struct rawhid_app_encoder_override *override = &encoder_overrides[active_layer_id][encoder_id];
    if (!override->valid) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_behavior_binding *binding =
        encoder_runtime_binding_for_direction(override, direction);
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
