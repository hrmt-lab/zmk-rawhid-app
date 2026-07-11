#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

struct rawhid_app_encoder_runtime_bindings {
    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    uint8_t source;
    uint8_t flags;
};

bool rawhid_app_encoder_runtime_get(uint32_t layer_id, uint8_t encoder_id,
                                    struct rawhid_app_encoder_runtime_bindings *bindings);
int rawhid_app_encoder_runtime_set(uint32_t layer_id, uint8_t encoder_id,
                                   const struct zmk_behavior_binding *cw_binding,
                                   const struct zmk_behavior_binding *ccw_binding);
bool rawhid_app_encoder_runtime_layer_exists(uint32_t layer_id);
bool rawhid_app_encoder_runtime_dirty(void);
int rawhid_app_encoder_runtime_save(void);
void rawhid_app_encoder_runtime_discard(void);
void rawhid_app_encoder_runtime_clear(uint32_t layer_id, uint8_t encoder_id);
