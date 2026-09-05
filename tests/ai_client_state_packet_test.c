#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

#include "ai_client_state_packet.h"

/* Records the slot the decoder resolved so every existing case can also assert
 * that legacy payloads keep landing on slot 0. */
static uint8_t decoded_slot;

static enum rawhid_app_ai_client_decode_result decode(const uint8_t *payload, uint8_t payload_len,
                                                      struct rawhid_app_ai_client_state *state) {
    decoded_slot = 0xff;
    return rawhid_app_ai_client_state_decode(payload, payload_len, state, &decoded_slot);
}

static void test_legacy_payload_defaults_to_unspecified(void) {
    const uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX, 1, 1,
                               RAWHID_APP_AI_ACTIVITY_WORKING, 0x34, 0x12};
    struct rawhid_app_ai_client_state state = {0};

    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_OK);
    assert(state.session_active);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 0x1234);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    assert(state.screenkey_state == RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_NORMAL);
    assert(decoded_slot == 0);
}

static void test_detailed_payload_accepts_every_known_phase(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX, 2, 1,
                         RAWHID_APP_AI_ACTIVITY_WORKING, 7, 0, 0};

    for (uint8_t phase = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
         phase <= RAWHID_APP_AI_WORK_PHASE_SEARCHING; phase++) {
        struct rawhid_app_ai_client_state state = {0};
        payload[6] = phase;
        assert(decode(payload, sizeof(payload), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(state.work_phase == phase);
        assert(decoded_slot == 0);
    }
}

static void test_unknown_phase_is_normalized_without_losing_base_state(void) {
    const uint8_t working[] = {RAWHID_APP_AI_CLIENT_CODEX, 3, 1,
                               RAWHID_APP_AI_ACTIVITY_WORKING, 9, 0, 0x7f};
    const uint8_t waiting[] = {RAWHID_APP_AI_CLIENT_CODEX, 3, 1,
                               RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, 10, 0, 0xfe};
    struct rawhid_app_ai_client_state state = {0};

    assert(decode(working, sizeof(working), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
    assert(state.revision == 9);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(decode(waiting, sizeof(waiting), &state) ==
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

    assert(decode(payload, 5, &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    assert(decode(payload, 9, &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);

    payload[6] = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
    payload[2] = 2;
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[2] = 0;
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[2] = 1;
    payload[3] = RAWHID_APP_AI_ACTIVITY_NONE;
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    payload[3] = 0xff;
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_INVALID);
}

static void test_claude_code_client_type_decodes_like_codex(void) {
    const uint8_t legacy[] = {RAWHID_APP_AI_CLIENT_CLAUDE_CODE, 1, 1,
                              RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL, 0x02, 0x01};
    uint8_t detailed[] = {RAWHID_APP_AI_CLIENT_CLAUDE_CODE, 1, 1,
                          RAWHID_APP_AI_ACTIVITY_WORKING, 5, 0, 0};
    struct rawhid_app_ai_client_state state = {0};

    assert(decode(legacy, sizeof(legacy), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_OK);
    assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL);
    assert(state.revision == 0x0102);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    for (uint8_t phase = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
         phase <= RAWHID_APP_AI_WORK_PHASE_SEARCHING; phase++) {
        detailed[6] = phase;
        assert(decode(detailed, sizeof(detailed), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
        assert(state.work_phase == phase);
    }

    detailed[6] = 0x40;
    assert(decode(detailed, sizeof(detailed), &state) ==
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
        assert(decode(payload, 6, &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_INVALID);
        assert(decode(payload, sizeof(payload), &state) ==
               RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    }
}

static void test_screenkey_state_payload_keeps_the_first_eight_bytes(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX,
                         1,
                         1,
                         RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL,
                         0x21,
                         0x43,
                         RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED,
                         3,
                         0};

    for (uint8_t screenkey_state = RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_NORMAL;
         screenkey_state <= RAWHID_APP_AI_CLIENT_SCREENKEY_STATE_SENT; screenkey_state++) {
        struct rawhid_app_ai_client_state state = {0};
        payload[8] = screenkey_state;
        assert(decode(payload, sizeof(payload), &state) == RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(decoded_slot == 3);
        assert(state.client_type == RAWHID_APP_AI_CLIENT_CODEX);
        assert(state.revision == 0x4321);
        assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
        assert(state.screenkey_state == screenkey_state);
    }

    payload[8] = 4;
    struct rawhid_app_ai_client_state invalid = {0};
    assert(decode(payload, sizeof(payload), &invalid) == RAWHID_APP_AI_CLIENT_DECODE_INVALID);
}

static void test_slot_payload_accepts_every_valid_slot(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX,
                         1,
                         1,
                         RAWHID_APP_AI_ACTIVITY_WORKING,
                         0x21,
                         0x43,
                         RAWHID_APP_AI_WORK_PHASE_EXECUTING,
                         0};

    for (uint8_t slot = 0; slot <= RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_MAX; slot++) {
        struct rawhid_app_ai_client_state state = {0};
        payload[7] = slot;
        assert(decode(payload, sizeof(payload), &state) == RAWHID_APP_AI_CLIENT_DECODE_OK);
        assert(decoded_slot == slot);
        /* The first seven bytes keep their legacy meaning. */
        assert(state.client_type == RAWHID_APP_AI_CLIENT_CODEX);
        assert(state.session_active);
        assert(state.activity_state == RAWHID_APP_AI_ACTIVITY_WORKING);
        assert(state.revision == 0x4321);
        assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    }
}

static void test_rejects_out_of_range_slots(void) {
    const uint8_t out_of_range[] = {8, 9, 0x80, 0xff};
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CLAUDE_CODE,
                         1,
                         1,
                         RAWHID_APP_AI_ACTIVITY_AVAILABLE,
                         1,
                         0,
                         RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED,
                         0};

    for (size_t index = 0; index < sizeof(out_of_range); index++) {
        struct rawhid_app_ai_client_state state = {0};
        payload[7] = out_of_range[index];
        assert(decode(payload, sizeof(payload), &state) == RAWHID_APP_AI_CLIENT_DECODE_INVALID);
    }
}

static void test_slot_payload_still_validates_the_legacy_fields(void) {
    uint8_t payload[] = {RAWHID_APP_AI_CLIENT_CODEX,
                         1,
                         1,
                         RAWHID_APP_AI_ACTIVITY_AVAILABLE,
                         1,
                         0,
                         RAWHID_APP_AI_WORK_PHASE_THINKING,
                         1};
    struct rawhid_app_ai_client_state state = {0};

    /* work phase outside WORKING is rejected whatever the slot says. */
    assert(decode(payload, sizeof(payload), &state) == RAWHID_APP_AI_CLIENT_DECODE_INVALID);

    /* An unknown work phase is still normalized rather than dropped, and the
     * slot survives the normalization. */
    payload[3] = RAWHID_APP_AI_ACTIVITY_WORKING;
    payload[6] = 0x7f;
    payload[7] = 3;
    assert(decode(payload, sizeof(payload), &state) ==
           RAWHID_APP_AI_CLIENT_DECODE_NORMALIZED_WORK_PHASE);
    assert(state.work_phase == RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    assert(decoded_slot == 3);

    /* Unknown client types are rejected at 8 bytes too. */
    payload[0] = 0x03;
    payload[6] = RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED;
    assert(decode(payload, sizeof(payload), &state) == RAWHID_APP_AI_CLIENT_DECODE_INVALID);
}

int main(void) {
    test_legacy_payload_defaults_to_unspecified();
    test_detailed_payload_accepts_every_known_phase();
    test_unknown_phase_is_normalized_without_losing_base_state();
    test_rejects_invalid_lengths_and_combinations();
    test_claude_code_client_type_decodes_like_codex();
    test_rejects_unknown_client_types();
    test_screenkey_state_payload_keeps_the_first_eight_bytes();
    test_slot_payload_accepts_every_valid_slot();
    test_rejects_out_of_range_slots();
    test_slot_payload_still_validates_the_legacy_fields();
    return 0;
}
