#include <string.h>

#include <rawhid_app/packet.h>

#include "ai_client_state_model.h"

static bool state_is_valid(const struct rawhid_app_ai_client_state *state) {
    return state->client_type == RAWHID_APP_AI_CLIENT_CODEX &&
           state->activity_state <= RAWHID_APP_AI_ACTIVITY_ERROR &&
           ((!state->session_active &&
             state->activity_state == RAWHID_APP_AI_ACTIVITY_NONE) ||
            (state->session_active &&
             state->activity_state != RAWHID_APP_AI_ACTIVITY_NONE));
}

static bool state_equals(const struct rawhid_app_ai_client_state *left,
                         const struct rawhid_app_ai_client_state *right) {
    return left->client_type == right->client_type &&
           left->client_variant == right->client_variant &&
           left->session_active == right->session_active &&
           left->activity_state == right->activity_state && left->revision == right->revision;
}

enum rawhid_app_ai_client_apply_result rawhid_app_ai_client_state_model_apply(
    struct rawhid_app_ai_client_state_model *model,
    const struct rawhid_app_ai_client_state *next) {
    if (!state_is_valid(next)) {
        return RAWHID_APP_AI_CLIENT_REJECTED;
    }
    if (model->valid && state_equals(&model->state, next)) {
        return RAWHID_APP_AI_CLIENT_HEARTBEAT;
    }

    const bool same_revision = model->valid && model->state.revision == next->revision;
    model->state = *next;
    model->valid = true;
    model->generation++;
    return same_revision ? RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION
                         : RAWHID_APP_AI_CLIENT_UPDATED;
}

bool rawhid_app_ai_client_state_model_timeout(struct rawhid_app_ai_client_state_model *model) {
    if (!model->valid) {
        return false;
    }
    memset(&model->state, 0, sizeof(model->state));
    model->valid = false;
    model->generation++;
    return true;
}
