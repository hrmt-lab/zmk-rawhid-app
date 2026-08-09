#pragma once

#include <stdint.h>

#include <rawhid_app/ai_client_state.h>

enum rawhid_app_ai_client_decode_result {
    RAWHID_APP_AI_CLIENT_DECODE_INVALID,
    RAWHID_APP_AI_CLIENT_DECODE_OK,
    RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE,
};

/* Decodes a 6, 7 or 8 byte AI_CLIENT_STATE payload.
 *
 * display_slot is returned separately from the state because it selects which
 * per-slot model the update belongs to rather than being part of the state
 * itself. 6 and 7 byte payloads always resolve to slot 0. */
enum rawhid_app_ai_client_decode_result
rawhid_app_ai_client_state_decode(const uint8_t *payload, uint8_t payload_len,
                                  struct rawhid_app_ai_client_state *state,
                                  uint8_t *display_slot);
