#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <rawhid_app/packet.h>

/* Pure AI client contract helpers shared by the packet decoder, the state model
 * and the DEVICE_HELLO capability bits. Kept free of Zephyr dependencies so the
 * host C tests can cover them. */

/* Client types the Core accepts. Accepting a type is independent from the
 * capability bits: capabilities advertise which clients a target can actually
 * represent, not which values the decoder lets through. */
static inline bool rawhid_app_ai_client_type_is_known(uint8_t client_type) {
    return client_type == RAWHID_APP_AI_CLIENT_CODEX ||
           client_type == RAWHID_APP_AI_CLIENT_CLAUDE_CODE;
}

/* AI client capability bits for DEVICE_HELLO.
 *
 * bit 10 / bit 11 need the Core plus a declared renderer. bit 12 additionally
 * needs a renderer that can represent Claude Code, and is never advertised on
 * its own. */
static inline uint32_t rawhid_app_ai_client_capability_bits(bool core_enabled,
                                                            bool renderer_enabled,
                                                            bool claude_code_renderer_enabled) {
    if (!core_enabled || !renderer_enabled) {
        return 0;
    }

    uint32_t caps = RAWHID_APP_CAP_AI_CLIENT_STATE | RAWHID_APP_CAP_AI_CLIENT_WORK_PHASE;
    if (claude_code_renderer_enabled) {
        caps |= RAWHID_APP_CAP_AI_CLIENT_CLAUDE_CODE;
    }

    return caps;
}
