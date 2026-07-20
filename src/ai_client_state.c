#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/ai_client_state.h>
#include <rawhid_app/packet.h>

#include "ai_client_state_model.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AI_CLIENT_HOST_TIMEOUT K_SECONDS(15)

static K_MUTEX_DEFINE(state_mutex);
static struct rawhid_app_ai_client_state_model state_model;

static void host_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&state_mutex, K_FOREVER);
    if (rawhid_app_ai_client_state_model_timeout(&state_model)) {
        LOG_INF("AI client state timed out");
    }
    k_mutex_unlock(&state_mutex);
}

K_WORK_DELAYABLE_DEFINE(host_timeout_work, host_timeout_handler);

bool rawhid_app_ai_client_state_get(struct rawhid_app_ai_client_state *state,
                                    uint32_t *generation) {
    bool valid;

    k_mutex_lock(&state_mutex, K_FOREVER);
    valid = state_model.valid;
    if (state != NULL) {
        *state = state_model.state;
    }
    if (generation != NULL) {
        *generation = state_model.generation;
    }
    k_mutex_unlock(&state_mutex);
    return valid;
}

void rawhid_app_ai_client_state_handle(const struct rawhid_app_packet *packet) {
    const uint8_t client_type = packet->ai_client_state.client_type;
    const uint8_t client_variant = packet->ai_client_state.client_variant;
    const uint8_t session_active = packet->ai_client_state.session_active;
    const uint8_t activity_state = packet->ai_client_state.activity_state;

    if (session_active > 1) {
        LOG_WRN("dropping invalid AI client state");
        return;
    }

    if (client_variant < 0x01 || client_variant > 0x03) {
        LOG_WRN("accepting unknown Codex client variant 0x%02x", client_variant);
    }

    const struct rawhid_app_ai_client_state next = {
        .client_type = client_type,
        .client_variant = client_variant,
        .session_active = session_active != 0,
        .activity_state = activity_state,
        .revision = packet->ai_client_state.revision,
    };

    k_mutex_lock(&state_mutex, K_FOREVER);
    const enum rawhid_app_ai_client_apply_result result =
        rawhid_app_ai_client_state_model_apply(&state_model, &next);
    if (result == RAWHID_APP_AI_CLIENT_REJECTED) {
        LOG_WRN("dropping invalid AI client state");
        k_mutex_unlock(&state_mutex);
        return;
    }
    if (result == RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION) {
        LOG_WRN("AI client payload changed without revision change");
    }
    if (result == RAWHID_APP_AI_CLIENT_UPDATED ||
        result == RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION) {
        LOG_INF("AI client state accepted: active=%u activity=%u revision=%u generation=%u",
                next.session_active, next.activity_state, next.revision, state_model.generation);
    } else {
        LOG_DBG("AI client state heartbeat: revision=%u", next.revision);
    }
    k_mutex_unlock(&state_mutex);

    k_work_reschedule(&host_timeout_work, AI_CLIENT_HOST_TIMEOUT);
}
