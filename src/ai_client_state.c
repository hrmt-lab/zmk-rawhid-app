#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/ai_client_state.h>
#include <rawhid_app/events/ai_client_state_changed.h>
#include <rawhid_app/packet.h>

#include "ai_client_state_model.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AI_CLIENT_HOST_TIMEOUT K_SECONDS(15)

static K_MUTEX_DEFINE(state_mutex);
static struct rawhid_app_ai_client_state_model state_model;

static void raise_state_event(const struct rawhid_app_ai_client_state *state,
                              uint32_t generation,
                              enum rawhid_app_ai_client_state_event_reason reason) {
    const int err = raise_rawhid_app_ai_client_state_changed(
        (struct rawhid_app_ai_client_state_changed){
            .state = *state,
            .state_generation = generation,
            .reason = reason,
        });
    if (err < 0) {
        LOG_WRN("AI client state event delivery failed: %d", err);
    }
}

static void host_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    bool timed_out;

    k_mutex_lock(&state_mutex, K_FOREVER);
    timed_out = rawhid_app_ai_client_state_model_timeout(&state_model);
    if (timed_out) {
        state = state_model.state;
        generation = state_model.generation;
        LOG_INF("AI client state timed out");
    }
    k_mutex_unlock(&state_mutex);

    if (timed_out) {
        raise_state_event(&state, generation, RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT);
    }
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
    const uint8_t work_phase = packet->ai_client_state.work_phase;

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
        .work_phase = work_phase,
    };

    uint32_t generation = 0;
    bool state_changed = false;

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
        result == RAWHID_APP_AI_CLIENT_UPDATED_WORK_PHASE ||
        result == RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION) {
        generation = state_model.generation;
        state_changed = true;
        LOG_INF("AI client state accepted: active=%u activity=%u phase=%u revision=%u generation=%u",
                next.session_active, next.activity_state, next.work_phase, next.revision,
                state_model.generation);
    } else {
        LOG_DBG("AI client state heartbeat: revision=%u", next.revision);
    }
    k_mutex_unlock(&state_mutex);

    if (state_changed) {
        const enum rawhid_app_ai_client_state_event_reason reason =
            next.session_active ? RAWHID_APP_AI_CLIENT_STATE_UPDATED
                                : RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED;
        raise_state_event(&next, generation, reason);
    }

    k_work_reschedule(&host_timeout_work, AI_CLIENT_HOST_TIMEOUT);
}
