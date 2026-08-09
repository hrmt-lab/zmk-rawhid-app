#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/ai_client_state.h>
#include <rawhid_app/events/ai_client_state_changed.h>
#include <rawhid_app/packet.h>

#include "ai_client_contract.h"
#include "ai_client_state_model.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AI_CLIENT_HOST_TIMEOUT K_SECONDS(15)

/* One independent model per logical display slot. Slot 0 is the legacy
 * single-screen destination used by 6 and 7 byte payloads. A slot only exists
 * on targets that declare that many physical renderers; updates addressed to a
 * slot this target does not have are dropped without touching any other slot.
 * Each slot also owns its host timeout, so an idle slot expires on its own
 * without disturbing a slot that is still being updated. */
struct ai_client_slot {
    struct rawhid_app_ai_client_state_model model;
    struct k_work_delayable host_timeout_work;
};

static K_MUTEX_DEFINE(state_mutex);
static struct ai_client_slot slots[RAWHID_APP_AI_CLIENT_SLOT_COUNT];
static bool slots_initialized;

static void raise_state_event(const struct rawhid_app_ai_client_state *state, uint8_t display_slot,
                              uint32_t generation,
                              enum rawhid_app_ai_client_state_event_reason reason) {
    const int err = raise_rawhid_app_ai_client_state_changed(
        (struct rawhid_app_ai_client_state_changed){
            .state = *state,
            .display_slot = display_slot,
            .state_generation = generation,
            .reason = reason,
        });
    if (err < 0) {
        LOG_WRN("AI client state event delivery failed: %d", err);
    }
}

static void host_timeout_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct ai_client_slot *slot = CONTAINER_OF(delayable, struct ai_client_slot, host_timeout_work);
    const uint8_t display_slot = (uint8_t)(slot - slots);

    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    bool timed_out;

    k_mutex_lock(&state_mutex, K_FOREVER);
    timed_out = rawhid_app_ai_client_state_model_timeout(&slot->model);
    if (timed_out) {
        state = slot->model.state;
        generation = slot->model.generation;
        LOG_INF("AI client state timed out: slot=%u", display_slot);
    }
    k_mutex_unlock(&state_mutex);

    if (timed_out) {
        raise_state_event(&state, display_slot, generation,
                          RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT);
    }
}

/* Must be called with state_mutex held. The timeout work items cannot be
 * initialized statically because they live in an array sized by Kconfig. */
static void ensure_slots_initialized(void) {
    if (slots_initialized) {
        return;
    }

    for (size_t index = 0; index < ARRAY_SIZE(slots); index++) {
        k_work_init_delayable(&slots[index].host_timeout_work, host_timeout_handler);
    }
    slots_initialized = true;
}

bool rawhid_app_ai_client_state_get_slot(uint8_t display_slot,
                                         struct rawhid_app_ai_client_state *state,
                                         uint32_t *generation) {
    if (display_slot >= ARRAY_SIZE(slots)) {
        return false;
    }

    const struct rawhid_app_ai_client_state_model *model = &slots[display_slot].model;
    bool valid;

    k_mutex_lock(&state_mutex, K_FOREVER);
    valid = model->valid;
    if (state != NULL) {
        *state = model->state;
    }
    if (generation != NULL) {
        *generation = model->generation;
    }
    k_mutex_unlock(&state_mutex);
    return valid;
}

bool rawhid_app_ai_client_state_get(struct rawhid_app_ai_client_state *state,
                                    uint32_t *generation) {
    return rawhid_app_ai_client_state_get_slot(0, state, generation);
}

void rawhid_app_ai_client_state_handle(const struct rawhid_app_packet *packet) {
    const uint8_t client_type = packet->ai_client_state.client_type;
    const uint8_t client_variant = packet->ai_client_state.client_variant;
    const uint8_t session_active = packet->ai_client_state.session_active;
    const uint8_t activity_state = packet->ai_client_state.activity_state;
    const uint8_t work_phase = packet->ai_client_state.work_phase;
    const uint8_t display_slot = packet->ai_client_state.display_slot;

    if (session_active > 1) {
        LOG_WRN("dropping invalid AI client state");
        return;
    }

    if (display_slot >= ARRAY_SIZE(slots)) {
        LOG_DBG("ignoring AI client state for absent display slot %u", display_slot);
        return;
    }

    if (client_variant < 0x01 || client_variant > 0x03) {
        LOG_WRN("accepting unknown client variant 0x%02x for client type 0x%02x", client_variant,
                client_type);
    }

    const struct rawhid_app_ai_client_state next = {
        .client_type = client_type,
        .client_variant = client_variant,
        .session_active = session_active != 0,
        .activity_state = activity_state,
        .revision = packet->ai_client_state.revision,
        .work_phase = work_phase,
    };

    struct ai_client_slot *slot = &slots[display_slot];
    uint32_t generation = 0;
    bool state_changed = false;

    k_mutex_lock(&state_mutex, K_FOREVER);
    ensure_slots_initialized();
    const enum rawhid_app_ai_client_apply_result result =
        rawhid_app_ai_client_state_model_apply(&slot->model, &next);
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
        generation = slot->model.generation;
        state_changed = true;
        LOG_INF("AI client state accepted: slot=%u active=%u activity=%u phase=%u revision=%u "
                "generation=%u",
                display_slot, next.session_active, next.activity_state, next.work_phase,
                next.revision, slot->model.generation);
    } else {
        LOG_DBG("AI client state heartbeat: slot=%u revision=%u", display_slot, next.revision);
    }
    k_mutex_unlock(&state_mutex);

    if (state_changed) {
        const enum rawhid_app_ai_client_state_event_reason reason =
            next.session_active ? RAWHID_APP_AI_CLIENT_STATE_UPDATED
                                : RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED;
        raise_state_event(&next, display_slot, generation, reason);
    }

    k_work_reschedule(&slot->host_timeout_work, AI_CLIENT_HOST_TIMEOUT);
}
