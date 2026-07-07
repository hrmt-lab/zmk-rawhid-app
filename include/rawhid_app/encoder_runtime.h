#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

struct rawhid_app_encoder_runtime_bindings {
    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    bool dirty;
};

bool rawhid_app_encoder_runtime_get(uint32_t layer_id, uint8_t encoder_id,
                                    struct rawhid_app_encoder_runtime_bindings *bindings);
void rawhid_app_encoder_runtime_set(uint32_t layer_id, uint8_t encoder_id,
                                    const struct zmk_behavior_binding *cw_binding,
                                    const struct zmk_behavior_binding *ccw_binding);
