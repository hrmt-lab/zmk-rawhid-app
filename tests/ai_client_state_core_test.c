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

static void reset_fixture(void);

static struct rawhid_app_packet packet_for_slot(uint8_t display_slot, uint8_t client_type,
                                                uint8_t activity, bool session_active,
                                                uint16_t revision, uint8_t work_phase) {
    return (struct rawhid_app_packet){
        .type = RAWHID_APP_PACKET_STATE_UPDATE,
        .ai_client_state =
            {
                .client_type = client_type,
                .client_variant = 0x01,
                .session_active = session_active,
                .activity_state = activity,
                .revision = revision,
                .work_phase = work_phase,
                .display_slot = display_slot,
            },
    };
}

static struct rawhid_app_packet packet_for_client(uint8_t client_type, uint8_t activity,
                                                  bool session_active, uint16_t revision,
                                                  uint8_t work_phase) {
    /* 6 and 7 byte payloads always resolve to slot 0. */
    return packet_for_slot(0, client_type, activity, session_active, revision, work_phase);
}

static struct rawhid_app_packet packet_for(uint8_t activity, bool session_active,
                                           uint16_t revision, uint8_t work_phase) {
    return packet_for_client(RAWHID_APP_AI_CLIENT_CODEX, activity, session_active, revision,
                             work_phase);
}

static void test_screenkey_state_change_is_published(void) {
    struct rawhid_app_ai_client_state state = {0};
    struct rawhid_app_packet waiting =
        packet_for(RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL, true, 30,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    struct rawhid_app_packet sent = waiting;
    sent.ai_client_state.screenkey_state = RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_SENT;

    reset_fixture();
    rawhid_app_ai_client_state_handle(&waiting);
    rawhid_app_ai_client_state_handle(&sent);
    assert(rawhid_app_ai_client_state_get(&state, NULL));
    assert(state.screenkey_state == RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_SENT);
    assert(event_count == 2);
    assert(last_event.state.screenkey_state == RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_SENT);
}

static void reset_fixture(void) {
    for (size_t index = 0; index < ARRAY_SIZE(slots); index++) {
        slots[index] = (struct ai_client_slot){0};
    }
    slots_initialized = false;
    event_count = 0;
    last_event = (struct rawhid_app_ai_client_state_changed){0};
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
    assert(slots[0].host_timeout_work.last_delay_ms == 15000);

    rawhid_app_ai_client_state_handle(&working);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(generation == 1);
    assert(event_count == 1);
    assert(slots[0].host_timeout_work.last_delay_ms == 15000);

    host_timeout_handler(&slots[0].host_timeout_work.work);
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

static void test_claude_code_client_type_is_published_and_dropped_when_unknown(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet claude =
        packet_for_client(RAWHID_APP_AI_CLIENT_CLAUDE_CODE, RAWHID_APP_AI_ACTIVITY_WORKING, true,
                          21, RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    struct rawhid_app_packet unknown_client =
        packet_for_client(0x03, RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 22,
                          RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&claude);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    assert(generation == 1);
    assert(event_count == 1);
    assert(last_event.state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_UPDATED);

    rawhid_app_ai_client_state_handle(&unknown_client);
    assert(rawhid_app_ai_client_state_get(&state, &generation));
    assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(generation == 1);
    assert(event_count == 1);
}

static void test_legacy_payloads_land_on_slot_zero(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet legacy =
        packet_for(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 30,
                   RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    struct rawhid_app_packet detailed =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 31,
                   RAWHID_APP_AI_WORK_PHASE_THINKING);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&legacy);
    assert(last_event.display_slot == 0);
    assert(rawhid_app_ai_client_state_get_slot(0, &state, &generation));
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_AVAILABLE);

    rawhid_app_ai_client_state_handle(&detailed);
    assert(last_event.display_slot == 0);
    assert(rawhid_app_ai_client_state_get_slot(0, &state, &generation));
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_THINKING);
    assert(generation == 2);

    /* The slot 0 shorthand keeps returning the same state. */
    struct rawhid_app_ai_client_state shorthand = {0};
    uint32_t shorthand_generation = 0;
    assert(rawhid_app_ai_client_state_get(&shorthand, &shorthand_generation));
    assert(shorthand.work_phase == RAWHID_APP_AI_WORK_PHASE_THINKING);
    assert(shorthand_generation == generation);
}

static void test_absent_slots_are_ignored(void) {
    struct rawhid_app_ai_client_state state = {0};
    uint32_t generation = 0;
    struct rawhid_app_packet base =
        packet_for(RAWHID_APP_AI_ACTIVITY_WORKING, true, 40,
                   RAWHID_APP_AI_WORK_PHASE_EXECUTING);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&base);
    assert(event_count == 1);

    /* Every slot this build does not have is dropped without disturbing the
     * slots it does have. */
    for (uint8_t slot = ARRAY_SIZE(slots); slot <= RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_MAX; slot++) {
        struct rawhid_app_packet absent =
            packet_for_slot(slot, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_ERROR, true,
                            41, RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
        rawhid_app_ai_client_state_handle(&absent);
        assert(event_count == 1);
        assert(!rawhid_app_ai_client_state_get_slot(slot, NULL, NULL));
    }

    assert(rawhid_app_ai_client_state_get_slot(0, &state, &generation));
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(generation == 1);
}

#if RAWHID_APP_AI_CLIENT_SLOT_COUNT >= 2

static void test_slots_hold_independent_state(void) {
    struct rawhid_app_ai_client_state slot0 = {0};
    struct rawhid_app_ai_client_state slot1 = {0};
    uint32_t generation0 = 0;
    uint32_t generation1 = 0;
    struct rawhid_app_packet codex_working =
        packet_for_slot(0, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_WORKING, true, 100,
                        RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    struct rawhid_app_packet claude_waiting =
        packet_for_slot(1, RAWHID_APP_AI_CLIENT_CLAUDE_CODE,
                        RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL, true, 7,
                        RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&codex_working);
    assert(last_event.display_slot == 0);
    rawhid_app_ai_client_state_handle(&claude_waiting);
    assert(last_event.display_slot == 1);
    assert(event_count == 2);

    assert(rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
    assert(rawhid_app_ai_client_state_get_slot(1, &slot1, &generation1));
    assert(slot0.client_type == RAWHID_APP_AI_CLIENT_CODEX);
    assert(slot0.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(slot0.work_phase == RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    assert(slot0.revision == 100);
    assert(generation0 == 1);
    assert(slot1.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(slot1.activity_state == RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL);
    assert(slot1.revision == 7);
    assert(generation1 == 1);
}

static void test_slot_updates_do_not_disturb_the_other_slot(void) {
    struct rawhid_app_ai_client_state slot0 = {0};
    struct rawhid_app_ai_client_state slot1 = {0};
    uint32_t generation0 = 0;
    uint32_t generation1 = 0;
    struct rawhid_app_packet slot0_available =
        packet_for_slot(0, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 1,
                        RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    struct rawhid_app_packet slot1_available =
        packet_for_slot(1, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 1,
                        RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&slot0_available);
    rawhid_app_ai_client_state_handle(&slot1_available);
    assert(event_count == 2);

    /* Drive slot 1 through several updates; slot 0 must not move at all. */
    for (uint16_t revision = 2; revision <= 5; revision++) {
        struct rawhid_app_packet slot1_working =
            packet_for_slot(1, RAWHID_APP_AI_CLIENT_CLAUDE_CODE, RAWHID_APP_AI_ACTIVITY_WORKING,
                            true, revision, RAWHID_APP_AI_WORK_PHASE_THINKING);
        rawhid_app_ai_client_state_handle(&slot1_working);
        assert(last_event.display_slot == 1);
    }

    assert(rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
    assert(slot0.client_type == RAWHID_APP_AI_CLIENT_CODEX);
    assert(slot0.activity_state == RAWHID_APP_AI_ACTIVITY_AVAILABLE);
    assert(slot0.revision == 1);
    assert(generation0 == 1);

    assert(rawhid_app_ai_client_state_get_slot(1, &slot1, &generation1));
    assert(slot1.revision == 5);
    assert(generation1 == 5);

    /* And the reverse direction. */
    struct rawhid_app_packet slot0_error =
        packet_for_slot(0, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_ERROR, true, 9,
                        RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    rawhid_app_ai_client_state_handle(&slot0_error);
    assert(last_event.display_slot == 0);

    assert(rawhid_app_ai_client_state_get_slot(1, &slot1, &generation1));
    assert(slot1.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(slot1.revision == 5);
    assert(generation1 == 5);

    assert(rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
    assert(slot0.activity_state == RAWHID_APP_AI_ACTIVITY_ERROR);
    assert(generation0 == 2);
}

static void test_slot_session_end_and_timeout_are_independent(void) {
    struct rawhid_app_ai_client_state slot0 = {0};
    struct rawhid_app_ai_client_state slot1 = {0};
    uint32_t generation0 = 0;
    uint32_t generation1 = 0;
    struct rawhid_app_packet slot0_working =
        packet_for_slot(0, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_WORKING, true, 60,
                        RAWHID_APP_AI_WORK_PHASE_SEARCHING);
    struct rawhid_app_packet slot1_working =
        packet_for_slot(1, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_WORKING, true, 61,
                        RAWHID_APP_AI_WORK_PHASE_SEARCHING);
    struct rawhid_app_packet slot1_ended =
        packet_for_slot(1, RAWHID_APP_AI_CLIENT_CODEX, RAWHID_APP_AI_ACTIVITY_NONE, false, 62,
                        RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    reset_fixture();
    rawhid_app_ai_client_state_handle(&slot0_working);
    rawhid_app_ai_client_state_handle(&slot1_working);
    assert(slots[0].host_timeout_work.last_delay_ms == 15000);
    assert(slots[1].host_timeout_work.last_delay_ms == 15000);

    /* Ending the slot 1 session clears only slot 1. */
    rawhid_app_ai_client_state_handle(&slot1_ended);
    assert(last_event.display_slot == 1);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_SESSION_ENDED);
    assert(rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
    assert(slot0.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(generation0 == 1);

    /* Expiring slot 1 raises a slot 1 timeout and leaves slot 0 valid. */
    host_timeout_handler(&slots[1].host_timeout_work.work);
    assert(last_event.display_slot == 1);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT);
    assert(!rawhid_app_ai_client_state_get_slot(1, &slot1, &generation1));
    assert(rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
    assert(slot0.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(generation0 == 1);

    /* Expiring slot 0 afterwards reports slot 0. */
    host_timeout_handler(&slots[0].host_timeout_work.work);
    assert(last_event.display_slot == 0);
    assert(last_event.reason == RAWHID_APP_AI_CLIENT_STATE_HOST_TIMEOUT);
    assert(!rawhid_app_ai_client_state_get_slot(0, &slot0, &generation0));
}

#endif /* RAWHID_APP_AI_CLIENT_SLOT_COUNT >= 2 */

int main(void) {
    test_update_heartbeat_and_timeout();
    test_same_revision_payload_change_is_published();
    test_same_revision_work_phase_change_is_published_once();
    test_screenkey_state_change_is_published();
    test_session_end_is_accepted_then_heartbeats_and_restarts();
    test_claude_code_client_type_is_published_and_dropped_when_unknown();
    test_legacy_payloads_land_on_slot_zero();
    test_absent_slots_are_ignored();
#if RAWHID_APP_AI_CLIENT_SLOT_COUNT >= 2
    test_slots_hold_independent_state();
    test_slot_updates_do_not_disturb_the_other_slot();
    test_slot_session_end_and_timeout_are_independent();
#endif
    return 0;
}
