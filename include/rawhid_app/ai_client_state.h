#pragma once

#include <stdbool.h>
#include <stdint.h>

struct rawhid_app_ai_client_state {
    uint8_t client_type;
    uint8_t client_variant;
    bool session_active;
    uint8_t activity_state;
    uint16_t revision;
};

/* Copies the current valid state. Returns false before the first valid update
 * and after the 15 second Host timeout. */
bool rawhid_app_ai_client_state_get(struct rawhid_app_ai_client_state *state,
                                    uint32_t *state_generation);
