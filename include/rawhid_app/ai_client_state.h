#pragma once

#include <stdbool.h>
#include <stdint.h>

struct rawhid_app_ai_client_state {
    uint8_t client_type;
    uint8_t client_variant;
    bool session_active;
    uint8_t activity_state;
    uint16_t revision;
    uint8_t work_phase;
    uint8_t screenkey_state;
};

/* Copies the current valid state of one logical display slot. Returns false
 * before the first valid update for that slot, after its 15 second Host
 * timeout, and for any slot this target does not have. */
bool rawhid_app_ai_client_state_get_slot(uint8_t display_slot,
                                         struct rawhid_app_ai_client_state *state,
                                         uint32_t *state_generation);

/* Slot 0 shorthand, kept for single-screen renderers. */
bool rawhid_app_ai_client_state_get(struct rawhid_app_ai_client_state *state,
                                    uint32_t *state_generation);
