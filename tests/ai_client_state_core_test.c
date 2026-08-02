#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include <rawhid_app/events/ai_client_state_changed.h>
#include <rawhid_app/packet.h>

static unsigned int event_count;
static struct rawhid_app_ai_client_state_changed last_event;

int raise_rawhid_app_ai_client_state_changed(
    struct rawhid_app_ai_client_state_changed event) {
    event_count++;
    last_event = event;
    return 0;
}

#include "../../src/ai_client_state_model.c"
#include "../../src/ai_client_state.c"

static struct rawhid_app_packet packet_for(uint8_t activity, bool session_active,
                                           uint16_t revision, uint8_t work_phase) {
    return (struct rawhid_app_packet){
        .type = RAWHID_APP_PACKET_STATE_UPDATE,
        .ai_client_state =
            {
                .client_type = RAWHID_APP_AI_CLIENT_CODEX,
                .client_variant = 0x01,
                .session_active = session_active,
                .activity_state = activity,
                .revision = revision,
                .work_phase = work_phase,
            },
    };
}

static void reset_fixture(void) {
    state_model = (struct rawhid_app_ai_client_state_model){0};
    event_count = 0;
    last_event = (struct rawhid_app_ai_client_state_changed){0};
    host_timeout_work.last_delay_ms = 0;
}

static void test_update_heartbeat_and_timeout(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet working =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 1,
                   RAWHID_APP_AI_WORK_PHASE_THINKING);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&working);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_THINKING);
    assert(generation == 1);
    assert(event_count == 1);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_UPDATED);
    assert(last_event.state_generation == 1);
    assert(host_timeout_work.last_delay_ms == 15000);

    rawhid_app_ai_client_state_handle(&working);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(generation == 1);
    assert(event_count == 1);
    assert(host_timeout_work.last_delay_ms == 15000);

    host_timeout_handler(&host_timeout_work.work);
    assert(!rawhid_app_ai_client_state_get(&state, &generation));
    assert(generation == 2);
    assert(event_count == 2);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT);
    assert(last_event.state_generation == 2);
    assert(last_event.state.activity_state == RAWHID_APP_AI_ACTIVITY_NONE);
    assert(last_event.state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    rawhid_app_ai_client_state_handle(&working);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 1);
    assert(generation == 3);
    assert(event_count == 3);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_UPDATED);
    assert(last_event.state_generation == 3);
}

static void test_same_revision_payload_change_is_published(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet working =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 3,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    struct rawhid_app_packet waiting =
        packet_for(RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, true, 3,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&working);
    rawhid_app_ai_client_state_handle(&waiting);

    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WAITING_INPUT);
    assert(generation == 2);
    assert(event_count == 2);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_UPDATED);
    assert(last_event.state_generation == 2);
}

static void test_same_revision_work_phase_change_is_published_once(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet thinking =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 5,
                   RAWHID_APP_AI_WORK_PHASE_THINKING);
    struct rawhid_app_packet executing =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 5,
                   RAWHID_APP_AI_WORK_PHASE_EXECUTING);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&thinking);
    rawhid_app_ai_client_state_handle(&executing);

    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.revision == 5);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    assert(generation == 2);
    assert(event_count == 2);
    assert(last_event.state.work_phase == RAWHID_APP_AI_WORK_PHASE_EXECUTING);

    rawhid_app_ai_client_state_handle(&executing);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(generation == 2);
    assert(event_count == 2);
}

static void test_session_end_is_accepted_then_heartbeats_and_restarts(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet working =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 10,
                   RAWHID_APP_AI_WORK_PHASE_SEARCHING);
    struct rawhid_app_packet session_ended =
        packet_for(RAWHID_APP_AI_ACTIVITY_NONE, false, 11,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    struct rawhid_app_packet available =
        packet_for(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 12,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&working);
    rawhid_app_ai_client_state_handle(&session_ended);

    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(!state.session_active);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_NONE);
    assert(state.revision == 11);
    assert(generation == 2);
    assert(event_count == 2);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED);
    assert(last_event.state_generation == 2);

    rawhid_app_ai_client_state_handle(&session_ended);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(generation == 2);
    assert(event_count == 2);

    rawhid_app_ai_client_state_handle(&available);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.session_active);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_AVAILABLE);
    assert(state.revision == 12);
    assert(generation == 3);
    assert(event_count == 3);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_UPDATED);
    assert(last_event.state_generation == 3);
}

int main(void) {
    test_update_heartbeat_and_timeout();
    test_same_revision_payload_change_is_published();
    test_same_revision_work_phase_change_is_published_once();
    test_session_end_is_accepted_then_heartbeats_and_restarts();
    return 0;
}
