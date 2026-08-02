#pragma once

#include <stdint.h>

#include <rawhid_app/ai_client_state.h>

enum rawhid_app_ai_client_decode_result {
    RAWHID_APP_AI_CLIENT_DECODE_INVALID,
    RAWHID_APP_AI_CLIENT_DECODE_OK,
    RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE,
};

enum rawhid_app_ai_client_decode_result rawhid_app_ai_client_state_decode(
    const uint8_t *payload, uint8_t payload_len, struct rawhid_app_ai_client_state *state);
