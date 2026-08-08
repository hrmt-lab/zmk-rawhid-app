#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

#include "ai_client_state_model.h"

static struct rawhid_app_ai_client_state client_state(uint8_t client_type, uint8_t activity,
                                                      bool session_active, uint16_t revision,
                                                      uint8_t work_phase) {
    return (struct rawhid_app_ai_client_state){
        .client_type = client_type,
        .client_variant = 0x01,
        .session_active = session_active,
        .activity_state = activity,
        .revision = revision,
        .work_phase = work_phase,
    };
}

static struct rawhid_app_ai_client_state codex_state(uint8_t activity, bool session_active,
                                                      uint16_t revision, uint8_t work_phase) {
    return client_state(RAWHID_APP_AI_CLIENT_CODEX, activity, session_active, revision,
                        work_phase);
}

static struct rawhid_app_ai_client_state claude_state(uint8_t activity, bool session_active,
                                                       uint16_t revision, uint8_t work_phase) {
    return client_state(RAWHID_APP_AI_CLIENT_CLAUDE_CODE, activity, session_active, revision,
                        work_phase);
}

static void assert_state(const struct rawhid_app_ai_client_state_model *model, bool valid,
                         uint32_t generation, uint8_t activity, uint16_t revision,
                         uint8_t work_phase) {
    assert(model->valid == valid);
    assert(model->generation == generation);
    if (valid) {
        assert(model->state.client_type == RAWHID_APP_AI_CLIENT_CODEX);
        assert(model->state.activity_state == activity);
        assert(model->state.revision == revision);
        assert(model->state.work_phase == work_phase);
    }
}

static void test_rejects_invalid_state_without_mutating_model(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    struct rawhid_app_ai_client_state invalid =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, false, 1,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &invalid) ==
           RAWHID_APP_AI_CLIENT_REJECTED);
    assert_state(&model, false, 0, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    assert(!rawhid_app_ai_client_state_model_timeout(&model));
    assert_state(&model, false, 0, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_heartbeat_keeps_generation(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state working =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 7,
                    RAWHID_APP_AI_WORK_PHASE_THINKING);

    assert(rawhid_app_ai_client_state_model_apply(&model, &working) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert_state(&model, true, 1, RAWHID_APP_AI_ACTIVITY_WORKING, 7,
                 RAWHID_APP_AI_WORK_PHASE_THINKING);

    assert(rawhid_app_ai_client_state_model_apply(&model, &working) ==
           RAWHID_APP_AI_CLIENT_HEARTBEAT);
    assert_state(&model, true, 1, RAWHID_APP_AI_ACTIVITY_WORKING, 7,
                 RAWHID_APP_AI_WORK_PHASE_THINKING);
}

static void test_arrival_order_lww_updates_generation(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state working =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 7,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    const struct rawhid_app_ai_client_state waiting =
        codex_state(RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, true, 7,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    const struct rawhid_app_ai_client_state completed =
        codex_state(RAWHID_APP_AI_ACTIVITY_COMPLETED, true, 8,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &working) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(rawhid_app_ai_client_state_model_apply(&model, &waiting) ==
           RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION);
    assert_state(&model, true, 2, RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, 7,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &completed) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert_state(&model, true, 3, RAWHID_APP_AI_ACTIVITY_COMPLETED, 8,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_revision_decrease_is_accepted_as_new_arrival(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state old_host =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 60000,
                    RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    const struct rawhid_app_ai_client_state restarted_host =
        codex_state(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 4,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &old_host) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(rawhid_app_ai_client_state_model_apply(&model, &restarted_host) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert_state(&model, true, 2, RAWHID_APP_AI_ACTIVITY_AVAILABLE, 4,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_timeout_clears_state_and_advances_generation(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state available =
        codex_state(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 2,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &available) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(rawhid_app_ai_client_state_model_timeout(&model));
    assert_state(&model, false, 2, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    assert(!rawhid_app_ai_client_state_model_timeout(&model));
    assert_state(&model, false, 2, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_same_revision_work_phase_change_is_an_update(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state thinking =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 9,
                    RAWHID_APP_AI_WORK_PHASE_THINKING);
    const struct rawhid_app_ai_client_state executing =
        codex_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 9,
                    RAWHID_APP_AI_WORK_PHASE_EXECUTING);

    assert(rawhid_app_ai_client_state_model_apply(&model, &thinking) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(rawhid_app_ai_client_state_model_apply(&model, &executing) ==
           RAWHID_APP_AI_CLIENT_UPDATED_WORK_PHASE);
    assert_state(&model, true, 2, RAWHID_APP_AI_ACTIVITY_WORKING, 9,
                 RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    assert(rawhid_app_ai_client_state_model_apply(&model, &executing) ==
           RAWHID_APP_AI_CLIENT_HEARTBEAT);
    assert_state(&model, true, 2, RAWHID_APP_AI_ACTIVITY_WORKING, 9,
                 RAWHID_APP_AI_WORK_PHASE_EXECUTING);
}

static void test_rejects_work_phase_outside_working(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state invalid =
        codex_state(RAWHID_APP_AI_ACTIVITY_WAITING_INPUT, true, 1,
                    RAWHID_APP_AI_WORK_PHASE_THINKING);

    assert(rawhid_app_ai_client_state_model_apply(&model, &invalid) ==
           RAWHID_APP_AI_CLIENT_REJECTED);
    assert_state(&model, false, 0, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                 RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
}

static void test_claude_code_client_type_follows_the_codex_lifecycle(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state thinking =
        claude_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 11,
                     RAWHID_APP_AI_WORK_PHASE_THINKING);
    const struct rawhid_app_ai_client_state executing =
        claude_state(RAWHID_APP_AI_ACTIVITY_WORKING, true, 11,
                     RAWHID_APP_AI_WORK_PHASE_EXECUTING);
    const struct rawhid_app_ai_client_state invalid_phase =
        claude_state(RAWHID_APP_AI_ACTIVITY_WAITING_APPROVAL, true, 12,
                     RAWHID_APP_AI_WORK_PHASE_THINKING);

    assert(rawhid_app_ai_client_state_model_apply(&model, &thinking) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(model.state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(rawhid_app_ai_client_state_model_apply(&model, &thinking) ==
           RAWHID_APP_AI_CLIENT_HEARTBEAT);
    assert(rawhid_app_ai_client_state_model_apply(&model, &executing) ==
           RAWHID_APP_AI_CLIENT_UPDATED_WORK_PHASE);
    assert(model.generation == 2);
    assert(rawhid_app_ai_client_state_model_apply(&model, &invalid_phase) ==
           RAWHID_APP_AI_CLIENT_REJECTED);
    assert(model.generation == 2);

    assert(rawhid_app_ai_client_state_model_timeout(&model));
    assert(!model.valid);
    assert(model.state.client_type == 0);
}

static void test_client_type_change_is_a_full_update(void) {
    struct rawhid_app_ai_client_state_model model = {0};
    const struct rawhid_app_ai_client_state codex =
        codex_state(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 5,
                    RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    const struct rawhid_app_ai_client_state claude =
        claude_state(RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 5,
                     RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

    assert(rawhid_app_ai_client_state_model_apply(&model, &codex) ==
           RAWHID_APP_AI_CLIENT_UPDATED);
    assert(rawhid_app_ai_client_state_model_apply(&model, &claude) ==
           RAWHID_APP_AI_CLIENT_UPDATED_SAME_REVISION);
    assert(model.state.client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE);
    assert(model.generation == 2);
}

static void test_rejects_unknown_client_types(void) {
    const uint8_t unknown_types[] = {0x00, 0x03, 0xff};

    for (size_t index = 0; index < sizeof(unknown_types); index++) {
        struct rawhid_app_ai_client_state_model model = {0};
        const struct rawhid_app_ai_client_state unknown =
            client_state(unknown_types[index], RAWHID_APP_AI_ACTIVITY_AVAILABLE, true, 1,
                         RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);

        assert(rawhid_app_ai_client_state_model_apply(&model, &unknown) ==
               RAWHID_APP_AI_CLIENT_REJECTED);
        assert_state(&model, false, 0, RAWHID_APP_AI_ACTIVITY_NONE, 0,
                     RAWHID_APP_AI_WORK_PHASE_UNSPECIFIED);
    }
}

int main(void) {
    test_rejects_invalid_state_without_mutating_model();
    test_heartbeat_keeps_generation();
    test_arrival_order_lww_updates_generation();
    test_revision_decrease_is_accepted_as_new_arrival();
    test_timeout_clears_state_and_advances_generation();
    test_same_revision_work_phase_change_is_an_update();
    test_rejects_work_phase_outside_working();
    test_claude_code_client_type_follows_the_codex_lifecycle();
    test_client_type_change_is_a_full_update();
    test_rejects_unknown_client_types();
    return 0;
}
