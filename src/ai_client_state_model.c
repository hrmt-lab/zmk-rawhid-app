#include <string.h>

#include <rawhid_app/packet.h>

#include "ai_client_contract.h"
#include "ai_client_state_model.h"

static bool state_is_valid(const struct rawhid_app_ai_client_state *state) {
    return rawhid_app_ai_client_type_is_known(state->client_type) &&
           state->activity_state <= RAWHID_APP_AI_ACTIVITY_ERROR &&
           state->work_phase <= RAWHID_APP_AI_WORK_PHASE_SEARCHING &&
           ((!state->session_active &&
             state->activity_state == RAWHID_APP_AI_ACTIVITY_NONE &&
             state->work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED) ||
            (state->session_active &&
             state->activity_state != RAWHID_APP_AI_ACTIVITY_NONE &&
             (state->activity_state == RAWHID_APP_AI_ACTIVITY_WORKING ||
              state->work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED)));
}

static bool state_equals(const struct rawhid_app_ai_client_state *left,
                         const struct rawhid_app_ai_client_state *right) {
    return left->client_type == right->client_type &&
           left->client_variant == right->client_variant &&
           left->session_active == right->session_active &&
           left->activity_state == right->activity_state && left->revision == right->revision &&
           left->work_phase == right->work_phase;
}

static bool base_state_equals(const struct rawhid_app_ai_client_state *left,
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
    const bool work_phase_only = same_revision && base_state_equals(&model->state, next);
    model->state = *next;
    model->valid = true;
    model->generation++;
    if (work_phase_only) {
        return RAWHID_APP_AI_CLIENT_UPDATED_WORK_PHASE;
    }
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
