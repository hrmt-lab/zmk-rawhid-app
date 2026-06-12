#define DT_DRV_COMPAT zmk_behavior_host_action

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata action_id_metadata[] = {{
    .display_name = "Action ID",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = 255},
}};

static const struct behavior_parameter_value_metadata value_metadata[] = {{
    .display_name = "Value",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = 255},
}};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = action_id_metadata,
    .param1_values_len = ARRAY_SIZE(action_id_metadata),
    .param2_values = value_metadata,
    .param2_values_len = ARRAY_SIZE(value_metadata),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};
#endif

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    if (!IS_ENABLED(CONFIG_RAWHID_APP_HOST_ACTION)) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    rawhid_app_host_action_send((uint8_t)binding->param1, (uint8_t)binding->param2);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_host_action_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define HOST_ACTION_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_host_action_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HOST_ACTION_INST)

#endif

int rawhid_app_host_action_send(uint8_t action_id, uint8_t value) {
#if IS_ENABLED(CONFIG_RAWHID_APP_HOST_ACTION)
    uint8_t buf[RAWHID_APP_PACKET_SIZE];

    rawhid_app_uplink_prepare(buf, RAWHID_APP_PACKET_HOST_ACTION);
    buf[4] = action_id;
    buf[5] = value;
    buf[RAWHID_APP_OFFSET_SEQ] = rawhid_app_uplink_next_seq(RAWHID_APP_PACKET_HOST_ACTION);

    return rawhid_app_uplink_send(buf);
#else
    ARG_UNUSED(action_id);
    ARG_UNUSED(value);

    return 0;
#endif
}
