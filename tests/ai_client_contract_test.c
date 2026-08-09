#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

#include "ai_client_contract.h"

#define CAP_STATE RAWHID_APP_CAP_AI_CLIENT_STATE
#define CAP_WORK_PHASE RAWHID_APP_CAP_AI_CLIENT_WORK_PHASE
#define CAP_CLAUDE_CODE RAWHID_APP_CAP_AI_CLIENT_CLAUDE_CODE
#define CAP_DISPLAY_SLOT RAWHID_APP_CAP_AI_CLIENT_DISPLAY_SLOT

static void test_capability_bit_values(void) {
    assert(CAP_STATE == (1u << 10));
    assert(CAP_WORK_PHASE == (1u << 11));
    assert(CAP_CLAUDE_CODE == (1u << 12));
    assert(CAP_DISPLAY_SLOT == (1u << 13));
    assert(RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_MAX == 7);
}

static void test_core_only_advertises_nothing(void) {
    assert(rawhid_app_ai_client_capability_bits(false, false, false, false) == 0);
    assert(rawhid_app_ai_client_capability_bits(true, false, false, false) == 0);
}

static void test_core_and_renderer_advertise_state_and_work_phase(void) {
    assert(rawhid_app_ai_client_capability_bits(true, true, false, false) ==
           (CAP_STATE | CAP_WORK_PHASE));
}

static void test_claude_code_renderer_adds_bit_12(void) {
    assert(rawhid_app_ai_client_capability_bits(true, true, true, false) ==
           (CAP_STATE | CAP_WORK_PHASE | CAP_CLAUDE_CODE));
}

static void test_claude_code_bit_is_never_advertised_alone(void) {
    assert(rawhid_app_ai_client_capability_bits(false, false, true, false) == 0);
    assert(rawhid_app_ai_client_capability_bits(true, false, true, false) == 0);
    assert(rawhid_app_ai_client_capability_bits(false, true, true, false) == 0);
}

static void test_display_slot_renderer_adds_bit_13(void) {
    assert(rawhid_app_ai_client_capability_bits(true, true, false, true) ==
           (CAP_STATE | CAP_WORK_PHASE | CAP_DISPLAY_SLOT));
    assert(rawhid_app_ai_client_capability_bits(true, true, true, true) ==
           (CAP_STATE | CAP_WORK_PHASE | CAP_CLAUDE_CODE | CAP_DISPLAY_SLOT));
}

static void test_display_slot_bit_is_never_advertised_alone(void) {
    assert(rawhid_app_ai_client_capability_bits(false, false, false, true) == 0);
    assert(rawhid_app_ai_client_capability_bits(true, false, false, true) == 0);
    assert(rawhid_app_ai_client_capability_bits(false, true, false, true) == 0);
    assert(rawhid_app_ai_client_capability_bits(false, true, true, true) == 0);
}

static void test_display_slot_bit_always_comes_with_state_and_work_phase(void) {
    /* Every combination that advertises bit 13 must also advertise bits 10
     * and 11, which is what the host relies on to pick the 8 byte payload. */
    for (unsigned int combination = 0; combination < 16u; combination++) {
        const uint32_t caps = rawhid_app_ai_client_capability_bits(
            (combination & 1u) != 0, (combination & 2u) != 0, (combination & 4u) != 0,
            (combination & 8u) != 0);
        if ((caps & CAP_DISPLAY_SLOT) != 0) {
            assert((caps & CAP_STATE) != 0);
            assert((caps & CAP_WORK_PHASE) != 0);
        }
    }
}

static void test_legacy_device_without_slot_renderer_is_unchanged(void) {
    /* A device that does not declare the multi screen renderer advertises
     * exactly what it did before bit 13 existed. */
    assert((rawhid_app_ai_client_capability_bits(true, true, true, false) & CAP_DISPLAY_SLOT) == 0);
    assert((rawhid_app_ai_client_capability_bits(true, true, false, false) & CAP_DISPLAY_SLOT) == 0);
}

static void test_default_slot_count_is_single_screen(void) {
    assert(RAWHID_APP_AI_CLIENT_SLOT_COUNT >= 1);
    assert(RAWHID_APP_AI_CLIENT_SLOT_COUNT <= RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_MAX + 1);
}

static void test_known_client_types(void) {
    assert(rawhid_app_ai_client_type_is_known(RAWHID_APP_AI_CLIENT_CODEX));
    assert(rawhid_app_ai_client_type_is_known(RAWHID_APP_AI_CLIENT_CLAUDE_CODE));
    assert(RAWHID_APP_AI_CLIENT_CODEX == 0x01);
    assert(RAWHID_APP_AI_CLIENT_CLAUDE_CODE == 0x02);

    const uint8_t unknown_types[] = {0x00, 0x03, 0x10, 0xff};
    for (size_t index = 0; index < sizeof(unknown_types); index++) {
        assert(!rawhid_app_ai_client_type_is_known(unknown_types[index]));
    }
}

int main(void) {
    test_capability_bit_values();
    test_core_only_advertises_nothing();
    test_core_and_renderer_advertise_state_and_work_phase();
    test_claude_code_renderer_adds_bit_12();
    test_claude_code_bit_is_never_advertised_alone();
    test_display_slot_renderer_adds_bit_13();
    test_display_slot_bit_is_never_advertised_alone();
    test_display_slot_bit_always_comes_with_state_and_work_phase();
    test_legacy_device_without_slot_renderer_is_unchanged();
    test_default_slot_count_is_single_screen();
    test_known_client_types();
    return 0;
}
