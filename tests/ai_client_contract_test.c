#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

#include "ai_client_contract.h"

#define CAP_STATE RAWHID_APP_CAP_AI_CLIENT_STATE
#define CAP_WORK_PHASE RAWHID_APP_CAP_AI_CLIENT_WORK_PHASE
#define CAP_CLAUDE_CODE RAWHID_APP_CAP_AI_CLIENT_CLAUDE_CODE

static void test_capability_bit_values(void) {
    assert(CAP_STATE == (1u << 10));
    assert(CAP_WORK_PHASE == (1u << 11));
    assert(CAP_CLAUDE_CODE == (1u << 12));
}

static void test_core_only_advertises_nothing(void) {
    assert(rawhid_app_ai_client_capability_bits(false, false, false) == 0);
    assert(rawhid_app_ai_client_capability_bits(true, false, false) == 0);
}

static void test_core_and_renderer_advertise_state_and_work_phase(void) {
    assert(rawhid_app_ai_client_capability_bits(true, true, false) ==
           (CAP_STATE | CAP_WORK_PHASE));
}

static void test_claude_code_renderer_adds_bit_12(void) {
    assert(rawhid_app_ai_client_capability_bits(true, true, true) ==
           (CAP_STATE | CAP_WORK_PHASE | CAP_CLAUDE_CODE));
}

static void test_claude_code_bit_is_never_advertised_alone(void) {
    assert(rawhid_app_ai_client_capability_bits(false, false, true) == 0);
    assert(rawhid_app_ai_client_capability_bits(true, false, true) == 0);
    assert(rawhid_app_ai_client_capability_bits(false, true, true) == 0);
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
    test_known_client_types();
    return 0;
}
