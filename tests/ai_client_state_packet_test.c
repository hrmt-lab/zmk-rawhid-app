#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

#include "ai_client_state_packet.h"

static void test_legacy_payload_defaults_to_unspecified(void) {
    const uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX, 1, 1,
                               RAWHID_APP_AI_ACTIVITY_WORKING, 0x34, 0x12};
    struct rawhid_app_ai_client_state state = {0};

    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_OK);
    assert(state.session_active);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 0x1234);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_detailed_payload_accepts_every_known_phase(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX, 2, 1,
                         RAWHID_APP_AI_ACTIVITY_WORKING, 7, 0, 0};

    for (uint8_t phase = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
         phase <= RAWHID_APP_AI_WORK_PHASE_SEARCHING; phase++) {
        struct rawhid_app_ai_client_state state = {0};
        payload[6] = phase;
        assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(state.work_phase == phase);
    }
}

static void test_unknown_phase_is_normalized_without_losing_base_state(void) {
    const uint8_t working[] = {RAWHID_APP_AI_CLIENT_CODEX, 3, 1,
                               RAWHID_APP_AI_ACTIVITY_WORKING, 9, 0, 0x7f};
    const uint8_t waiting[] = {RAWHID_APP_AI_CLIENT_CODEX, 3, 1,
                               RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, 10, 0, 0xfe};
    struct rawhid_app_ai_client_state state = {0};

    assert(rawhid_app_ai_client_state_decode(working, sizeof(working), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 9);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_decode(waiting, sizeof(waiting), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WAITING_INPUT);
    assert(state.revision == 10);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_rejects_invalid_lengths_and_combinations(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX, 1, 1,
                         RAWHID_APP_AI_ACTIVITY_AVAILABLE, 1, 0,
                         RAWHID_APP_AI_WORK_PHASE_THINKING};
    struct rawhid_app_ai_client_state state = {0};

    assert(rawhid_app_ai_client_state_decode(payload, 5, &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    assert(rawhid_app_ai_client_state_decode(payload, 8, &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);

    payload[6] = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
    payload[2] = 2;
    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[2] = 0;
    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[2] = 1;
    payload[3] = RAWHID_APP_AI_ACTIVITY_NONE;
    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[3] = 0xff;
    assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
}

static void test_claude_code_client_type_decodes_like_codex(void) {
    const uint8_t legacy[] = {RAWHID_APP_AI_CLIENT_CLAUDE_CODE, 1, 1,
                              RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL, 0x02, 0x01};
    uint8_t detailed[] = {RAWHID_APP_AI_CLIENT_CLAUDE_CODE, 1, 1,
                          RAWHID_APP_AI_ACTIVITY_WORKING, 5, 0, 0};
    struct rawhid_app_ai_client_state state = {0};

    assert(rawhid_app_ai_client_state_decode(legacy, sizeof(legacy), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_OK);
    assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL);
    assert(state.revision == 0x0102);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    for (uint8_t phase = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
         phase <= RAWHID_APP_AI_WORK_PHASE_SEARCHING; phase++) {
        detailed[6] = phase;
        assert(rawhid_app_ai_client_state_decode(detailed, sizeof(detailed), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
        assert(state.work_phase == phase);
    }

    detailed[6] = 0x40;
    assert(rawhid_app_ai_client_state_decode(detailed, sizeof(detailed), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE);
    assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 5);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_rejects_unknown_client_types(void) {
    const uint8_t unknown_types[] = {0x00, 0x03, 0xff};
    uint8_t payload[] = {0, 1, 1, RAWHID_APP_AI_ACTIVITY_AVAILABLE, 1, 0,
                         RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED};
    struct rawhid_app_ai_client_state state = {0};

    for (size_t index = 0; index < sizeof(unknown_types); index++) {
        payload[0] = unknown_types[index];
        assert(rawhid_app_ai_client_state_decode(payload, 6, &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_INVALID);
        assert(rawhid_app_ai_client_state_decode(payload, sizeof(payload), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    }
}

int main(void) {
    test_legacy_payload_defaults_to_unspecified();
    test_detailed_payload_accepts_every_known_phase();
    test_unknown_phase_is_normalized_without_losing_base_state();
    test_rejects_invalid_lengths_and_combinations();
    test_claude_code_client_type_decodes_like_codex();
    test_rejects_unknown_client_types();
    return 0;
}
