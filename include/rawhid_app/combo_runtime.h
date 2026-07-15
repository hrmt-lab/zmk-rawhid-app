/*
 * Copyright (c) 2026 Keylink Studio Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define RAWHID_APP_COMBO_RUNTIME_MAX_SLOTS 32
#define RAWHID_APP_COMBO_RUNTIME_MAX_KEYS 8
#define RAWHID_APP_COMBO_RUNTIME_NAME_LEN 16

struct rawhid_app_combo_runtime_definition {
    char name[RAWHID_APP_COMBO_RUNTIME_NAME_LEN];
    uint8_t key_count;
    uint16_t key_positions[RAWHID_APP_COMBO_RUNTIME_MAX_KEYS];
    struct zmk_behavior_binding binding;
    uint32_t layer_mask; /* 0 means all layers. */
    uint16_t timeout_ms;
    uint16_t require_prior_idle_ms; /* 0 means disabled. */
    bool slow_release;
};

struct rawhid_app_combo_runtime_limits {
    uint8_t max_slots;
    uint8_t max_keys;
    uint8_t occupied_count;
};

struct rawhid_app_combo_runtime_diagnostics {
    uint8_t flags;
    uint32_t stale_slots;
    uint32_t invalid_slots;
};

/* Read-only Phase 1C API. Mutation is intentionally not connected yet. */
void rawhid_app_combo_runtime_get_limits(struct rawhid_app_combo_runtime_limits *limits);
uint32_t rawhid_app_combo_runtime_occupied_mask(void);
bool rawhid_app_combo_runtime_get(uint8_t slot,
                                  struct rawhid_app_combo_runtime_definition *definition);
int rawhid_app_combo_runtime_validate(const struct rawhid_app_combo_runtime_definition *definition,
                                      int replacing_slot);
bool rawhid_app_combo_runtime_keys_pressed(void);
bool rawhid_app_combo_runtime_idle(void);
void rawhid_app_combo_runtime_get_diagnostics(struct rawhid_app_combo_runtime_diagnostics *diagnostics);
/* Returns the saved wire item only for a structurally valid stale record. */
bool rawhid_app_combo_runtime_get_stale_item(uint8_t slot, uint8_t item[52]);
