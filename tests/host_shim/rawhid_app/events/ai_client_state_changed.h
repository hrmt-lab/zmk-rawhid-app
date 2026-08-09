#pragma once

#include <stdint.h>

#include <rawhid_app/ai_client_state.h>

enum rawhid_app_ai_client_state_event_reason {
    RAWHID_APP_AI_CLIENT_STATE_UPDATED,
    RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED,
    RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT,
};

struct rawhid_app_ai_client_state_changed {
    struct rawhid_app_ai_client_state state;
    uint8_t display_slot;
    uint32_t state_generation;
    enum rawhid_app_ai_client_state_event_reason reason;
};

int raise_rawhid_app_ai_client_state_changed(
    struct rawhid_app_ai_client_state_changed event);
