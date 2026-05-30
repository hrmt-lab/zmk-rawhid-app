#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <rawhid_app/ai_usage.h>
#include <rawhid_app/packet.h>

/* Two provider slots: index 0 = codex (1), index 1 = claude_code (2). */
#define AI_USAGE_SLOT_COUNT 2

static struct rawhid_app_ai_usage_provider providers[AI_USAGE_SLOT_COUNT];
static K_MUTEX_DEFINE(providers_lock);

static int slot_for_provider(uint8_t provider) {
    switch (provider) {
    case RAWHID_APP_AI_PROVIDER_CODEX:
        return 0;
    case RAWHID_APP_AI_PROVIDER_CLAUDE_CODE:
        return 1;
    default:
        return -1;
    }
}

void rawhid_app_ai_usage_handle(const struct rawhid_app_packet *packet) {
    int slot = slot_for_provider(packet->ai_usage.provider);
    if (slot < 0) {
        return;
    }

    if (k_mutex_lock(&providers_lock, K_MSEC(5)) < 0) {
        return;
    }

    providers[slot] = (struct rawhid_app_ai_usage_provider){
        .present = true,
        .provider = packet->ai_usage.provider,
        .flags = packet->ai_usage.flags,
        .five_hour_used_bp = packet->ai_usage.five_hour_used_bp,
        .seven_day_used_bp = packet->ai_usage.seven_day_used_bp,
        .five_hour_reset_unix = packet->ai_usage.five_hour_reset_unix,
        .seven_day_reset_unix = packet->ai_usage.seven_day_reset_unix,
        .updated_unix = packet->ai_usage.updated_unix,
        .error_code = packet->ai_usage.error_code,
        .received_uptime_ms = k_uptime_get(),
    };

    k_mutex_unlock(&providers_lock);
}

bool rawhid_app_ai_usage_get(uint8_t provider, struct rawhid_app_ai_usage_provider *out) {
    if (out == NULL) {
        return false;
    }

    int slot = slot_for_provider(provider);
    if (slot < 0) {
        out->present = false;
        return false;
    }

    if (k_mutex_lock(&providers_lock, K_MSEC(5)) < 0) {
        out->present = false;
        return false;
    }

    *out = providers[slot];
    k_mutex_unlock(&providers_lock);

    return out->present;
}
