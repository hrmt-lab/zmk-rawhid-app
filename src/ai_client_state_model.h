#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <rawhid_app/ai_client_state.h>

enum rawhid_app_ai_client_apply_result {
    RAWHID_APP_AI_CLIENT_REJECTED,
    RAWHID_APP_AI_CLIENT_HEARTBEAT,
    RAWHID_APP_AI_CLIENT_UPDATED,
    RAWHID_APP_AI_CLIENT_UPDATED_WORK_PHASE,
    RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION,
};

struct rawhid_app_ai_client_state_model {
    struct rawhid_app_ai_client_state state;
    bool valid;
    uint32_t generation;
};

enum rawhid_app_ai_client_apply_result rawhid_app_ai_client_state_model_apply(
    struct rawhid_app_ai_client_state_model *model,
    const struct rawhid_app_ai_client_state *next);

bool rawhid_app_ai_client_state_model_timeout(struct rawhid_app_ai_client_state_model *model);
