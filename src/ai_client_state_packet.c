#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rawhid_app/packet.h>

#include "ai_client_contract.h"
#include "ai_client_state_packet.h"

#define AI_CLIENT_STATE_LEGACY_PAYLOAD_LEN 6
#define AI_CLIENT_STATE_WORK_PHASE_PAYLOAD_LEN 7
#define AI_CLIENT_STATE_DISPLAY_SLOT_PAYLOAD_LEN 8

enum rawhid_app_ai_client_decode_result
rawhid_app_ai_client_state_decode(const uint8_t *payload, uint8_t payload_len,
                                  struct rawhid_app_ai_client_state *state,
                                  uint8_t *display_slot) {
    if (payload == NULL || state == NULL || display_slot == NULL ||
        (payload_len != AI_CLIENT_STATE_LEGACY_PAYLOAD_LEN &&
         payload_len != AI_CLIENT_STATE_WORK_PHASE_PAYLOAD_LEN &&
         payload_len != AI_CLIENT_STATE_DISPLAY_SLOT_PAYLOAD_LEN)) {
        return RAWHID_APP_AI_CLIENT_DECODE_INVALID;
    }

    memset(state, 0, sizeof(*state));
    *display_slot = 0;
    state->client_type = payload[0];
    state->client_variant = payload[1];
    state->session_active = payload[2] != 0;
    state->activity_state = payload[3];
    state->revision = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
    state->work_phase = payload_len >= AI_CLIENT_STATE_WORK_PHASE_PAYLOAD_LEN
                            ? payload[6]
                            : RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;

    if (payload_len == AI_CLIENT_STATE_DISPLAY_SLOT_PAYLOAD_LEN) {
        if (payload[7] > RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_MAX) {
            return RAWHID_APP_AI_CLIENT_DECODE_INVALID;
        }
        *display_slot = payload[7];
    }

    if (payload[2] > 1 || !rawhid_app_ai_client_type_is_known(state->client_type) ||
        state->activity_state > RAWHID_APP_AI_ACTIVITY_ERROR ||
        (!state->session_active && state->activity_state != RAWHID_APP_AI_ACTIVITY_NONE) ||
        (state->session_active && state->activity_state == RAWHID_APP_AI_ACTIVITY_NONE)) {
        return RAWHID_APP_AI_CLIENT_DECODE_INVALID;
    }

    enum rawhid_app_ai_client_decode_result result = RAWHID_APP_AI_CLIENT_DECODE_OK;
    if (state->work_phase > RAWHID_APP_AI_WORK_PHASE_SEARCHING) {
        state->work_phase = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
        result = RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE;
    }

    if (state->activity_state != RAWHID_APP_AI_ACTIVITY_WORKING &&
        state->work_phase != RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED) {
        return RAWHID_APP_AI_CLIENT_DECODE_INVALID;
    }

    return result;
}
