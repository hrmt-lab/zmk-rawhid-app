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

/* Logical display slots the Core can hold. Targets without the multi-screen
 * renderer keep a single slot so their RAM footprint is unchanged. */
#if defined(CONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT)
#define RAWHID_APP_AI_CLIENT_SLOT_COUNT CONFIG_RAWHID_APP_AI_CLIENT_DISPLAY_SLOT_COUNT
#else
#define RAWHID_APP_AI_CLIENT_SLOT_COUNT 1
#endif

/* AI client capability bits for DEVICE_HELLO.
 *
 * bit 10 / bit 11 need the Core plus a declared renderer. bit 12 additionally
 * needs a renderer that can represent Claude Code, bit 13 a renderer that
 * drives one physical screen per logical display slot, and bit 14 a renderer
 * that understands the ninth ScreenKey display-state byte. Neither is ever
 * advertised on its own. */
static inline uint32_t
rawhid_app_ai_client_capability_bits(bool core_enabled, bool renderer_enabled,
                                     bool claude_code_renderer_enabled,
                                     bool display_slot_renderer_enabled,
                                     bool screenkey_state_renderer_enabled) {
    if (!core_enabled || !renderer_enabled) {
        return 0;
    }

    uint32_t caps = RAWHID_APP_CAP_AI_CLIENT_STATE | RAWHID_APP_CAP_AI_CLIENT_WORK_PHASE;
    if (claude_code_renderer_enabled) {
        caps |= RAWHID_APP_CAP_AI_CLIENT_CLAUDE_CODE;
    }
    if (display_slot_renderer_enabled) {
        caps |= RAWHID_APP_CAP_AI_CLIENT_DISPLAY_SLOT;
    }
    if (display_slot_renderer_enabled && screenkey_state_renderer_enabled) {
        caps |= RAWHID_APP_CAP_AI_CLIENT_SCREENKEY_STATE;
    }

    return caps;
}
