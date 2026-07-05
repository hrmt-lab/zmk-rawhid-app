#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <raw_hid/events.h>

#include <zmk/event_manager.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/packet.h>
#include <rawhid_app/identity.h>
#include <rawhid_app/uplink.h>

/* DEVICE_HELLO payload offsets. */
#define RAWHID_APP_HELLO_PAYLOAD_LEN 12
#define RAWHID_APP_HELLO_CAPABILITIES 0   /* u32 LE */
#define RAWHID_APP_HELLO_DEVICE_UID_HASH 4 /* u64 LE */

/* APP_LAYER payload offsets. */
#define RAWHID_APP_APP_LAYER_PAYLOAD_LEN 2
#define RAWHID_APP_APP_LAYER_ACTION 0
#define RAWHID_APP_APP_LAYER_LAYER 1

/* TIME_SYNC payload offsets. */
#define RAWHID_APP_TIME_SYNC_PAYLOAD_LEN 9
#define RAWHID_APP_TIME_SYNC_UNIX_TIME_SEC 0
#define RAWHID_APP_TIME_SYNC_TZ_OFFSET_MIN 4
#define RAWHID_APP_TIME_SYNC_WEEKDAY 6
#define RAWHID_APP_TIME_SYNC_FORMAT_HINT 7
#define RAWHID_APP_TIME_SYNC_CLOCK_MODE 8

/* AI_USAGE payload offsets. */
#define RAWHID_APP_AI_PAYLOAD_LEN 19
#define RAWHID_APP_AI_PROVIDER 0
#define RAWHID_APP_AI_FLAGS 1
#define RAWHID_APP_AI_FIVE_HOUR_USED_BP 2
#define RAWHID_APP_AI_SEVEN_DAY_USED_BP 4
#define RAWHID_APP_AI_FIVE_HOUR_RESET 6
#define RAWHID_APP_AI_SEVEN_DAY_RESET 10
#define RAWHID_APP_AI_UPDATED 14
#define RAWHID_APP_AI_ERROR_CODE 18

BUILD_ASSERT(CONFIG_RAW_HID_REPORT_SIZE == RAWHID_APP_PACKET_SIZE,
             "rawhid-app requires 64 byte reports");

static uint8_t hello_response[RAWHID_APP_PACKET_SIZE];

static bool reserved_bytes_are_zero(const uint8_t *data, uint8_t start, uint8_t end_inclusive) {
    for (uint8_t i = start; i <= end_inclusive; i++) {
        if (data[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool packet_type_is_known(uint8_t packet_type) {
    switch (packet_type) {
    case RAWHID_APP_PACKET_HOST_HELLO:
    case RAWHID_APP_PACKET_DEVICE_HELLO:
    case RAWHID_APP_PACKET_ERROR:
    case RAWHID_APP_PACKET_PING:
    case RAWHID_APP_PACKET_PONG:
    case RAWHID_APP_PACKET_AI_USAGE:
    case RAWHID_APP_PACKET_TIME_SYNC:
    case RAWHID_APP_PACKET_APP_LAYER:
    case RAWHID_APP_PACKET_BATTERY_STATUS:
    case RAWHID_APP_PACKET_HOST_ACTION:
    case RAWHID_APP_PACKET_KEY_STATS:
    case RAWHID_APP_PACKET_LAYER_STATE:
    case RAWHID_APP_PACKET_KEY_PRESS:
    case RAWHID_APP_PACKET_CONFIG_REQUEST:
    case RAWHID_APP_PACKET_CONFIG_RESPONSE:
        return true;
    default:
        return false;
    }
}

static bool payload_padding_is_zero(const uint8_t *data, uint8_t payload_len) {
    if (payload_len >= RAWHID_APP_PAYLOAD_SIZE) {
        return true;
    }

    return reserved_bytes_are_zero(data, RAWHID_APP_OFFSET_PAYLOAD + payload_len,
                                   RAWHID_APP_PACKET_SIZE - 1);
}

static bool parse_app_layer_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    const uint8_t *payload = &data[RAWHID_APP_OFFSET_PAYLOAD];
    uint8_t action = payload[RAWHID_APP_APP_LAYER_ACTION];
    uint8_t layer = payload[RAWHID_APP_APP_LAYER_LAYER];

    if (action != RAWHID_APP_APP_LAYER_SET && action != RAWHID_APP_APP_LAYER_CLEAR) {
        return false;
    }

    if (action == RAWHID_APP_APP_LAYER_SET && layer > RAWHID_APP_MAX_LAYER) {
        return false;
    }

    packet->app_layer.action = action;
    packet->app_layer.layer = layer;
    packet->app_layer.seq = data[RAWHID_APP_OFFSET_SEQ];
    return true;
}

static bool parse_time_sync_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    const uint8_t *payload = &data[RAWHID_APP_OFFSET_PAYLOAD];
    uint8_t weekday = payload[RAWHID_APP_TIME_SYNC_WEEKDAY];
    uint8_t format_hint = payload[RAWHID_APP_TIME_SYNC_FORMAT_HINT];
    uint8_t clock_mode = payload[RAWHID_APP_TIME_SYNC_CLOCK_MODE];

    if (weekday < 1 || weekday > 7) {
        return false;
    }

    if (format_hint > RAWHID_APP_TIME_FORMAT_WEEKDAY_HM) {
        format_hint = RAWHID_APP_TIME_FORMAT_TIME_HM;
    }

    if (clock_mode > RAWHID_APP_CLOCK_12H) {
        clock_mode = RAWHID_APP_CLOCK_24H;
    }

    packet->time_sync.unix_time_sec = sys_get_le32(&payload[RAWHID_APP_TIME_SYNC_UNIX_TIME_SEC]);
    packet->time_sync.tz_offset_min =
        (int16_t)sys_get_le16(&payload[RAWHID_APP_TIME_SYNC_TZ_OFFSET_MIN]);
    packet->time_sync.weekday = weekday;
    packet->time_sync.format_hint = (enum rawhid_app_time_format)format_hint;
    packet->time_sync.clock_mode = (enum rawhid_app_clock_mode)clock_mode;
    return true;
}

static bool parse_ai_usage_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    const uint8_t *payload = &data[RAWHID_APP_OFFSET_PAYLOAD];
    uint8_t provider = payload[RAWHID_APP_AI_PROVIDER];

    if (provider != RAWHID_APP_AI_PROVIDER_CODEX &&
        provider != RAWHID_APP_AI_PROVIDER_CLAUDE_CODE) {
        return false;
    }

    uint16_t five_hour_bp = sys_get_le16(&payload[RAWHID_APP_AI_FIVE_HOUR_USED_BP]);
    uint16_t seven_day_bp = sys_get_le16(&payload[RAWHID_APP_AI_SEVEN_DAY_USED_BP]);

    packet->ai_usage.provider = provider;
    packet->ai_usage.flags = payload[RAWHID_APP_AI_FLAGS];
    packet->ai_usage.five_hour_used_bp = MIN(five_hour_bp, 10000);
    packet->ai_usage.seven_day_used_bp = MIN(seven_day_bp, 10000);
    packet->ai_usage.five_hour_reset_unix = sys_get_le32(&payload[RAWHID_APP_AI_FIVE_HOUR_RESET]);
    packet->ai_usage.seven_day_reset_unix = sys_get_le32(&payload[RAWHID_APP_AI_SEVEN_DAY_RESET]);
    packet->ai_usage.updated_unix = sys_get_le32(&payload[RAWHID_APP_AI_UPDATED]);
    packet->ai_usage.error_code = payload[RAWHID_APP_AI_ERROR_CODE];
    return true;
}

static bool parse_packet(const struct raw_hid_received_event *event,
                         struct rawhid_app_packet *packet) {
    if (event == NULL || event->data == NULL || event->length != RAWHID_APP_PACKET_SIZE) {
        return false;
    }

    const uint8_t *data = event->data;

    if (data[RAWHID_APP_OFFSET_MAGIC_0] != RAWHID_APP_MAGIC_0 ||
        data[RAWHID_APP_OFFSET_MAGIC_1] != RAWHID_APP_MAGIC_1) {
        return false;
    }

    if (data[RAWHID_APP_OFFSET_VERSION] != RAWHID_APP_VERSION) {
        return false;
    }

    if (!packet_type_is_known(data[RAWHID_APP_OFFSET_TYPE])) {
        return false;
    }

    if (!reserved_bytes_are_zero(data, RAWHID_APP_OFFSET_RESERVED_START,
                                 RAWHID_APP_OFFSET_RESERVED_END)) {
        return false;
    }

    uint8_t payload_len = data[RAWHID_APP_OFFSET_PAYLOAD_LEN];
    if (payload_len > RAWHID_APP_PAYLOAD_SIZE || !payload_padding_is_zero(data, payload_len)) {
        return false;
    }

    packet->type = data[RAWHID_APP_OFFSET_TYPE];

    switch (packet->type) {
    case RAWHID_APP_PACKET_HOST_HELLO:
        if (payload_len != 0) {
            return false;
        }
        packet->hello.seq = data[RAWHID_APP_OFFSET_SEQ];
        return true;
    case RAWHID_APP_PACKET_APP_LAYER:
        if (payload_len != RAWHID_APP_APP_LAYER_PAYLOAD_LEN) {
            return false;
        }
        return parse_app_layer_packet(data, packet);
    case RAWHID_APP_PACKET_TIME_SYNC:
        if (payload_len != RAWHID_APP_TIME_SYNC_PAYLOAD_LEN) {
            return false;
        }
        return parse_time_sync_packet(data, packet);
    case RAWHID_APP_PACKET_AI_USAGE:
        if (payload_len != RAWHID_APP_AI_PAYLOAD_LEN) {
            return false;
        }
        return parse_ai_usage_packet(data, packet);
    /* Reserved, direction-mismatched, or not implemented in this phase. */
    case RAWHID_APP_PACKET_DEVICE_HELLO:
    case RAWHID_APP_PACKET_ERROR:
    case RAWHID_APP_PACKET_PING:
    case RAWHID_APP_PACKET_PONG:
    case RAWHID_APP_PACKET_BATTERY_STATUS:
    case RAWHID_APP_PACKET_HOST_ACTION:
    case RAWHID_APP_PACKET_KEY_STATS:
    case RAWHID_APP_PACKET_LAYER_STATE:
    case RAWHID_APP_PACKET_KEY_PRESS:
    case RAWHID_APP_PACKET_CONFIG_REQUEST:
    case RAWHID_APP_PACKET_CONFIG_RESPONSE:
        return true;
    default:
        return false;
    }
}

static void send_device_hello(uint8_t seq) {
    memset(hello_response, 0, sizeof(hello_response));
    hello_response[RAWHID_APP_OFFSET_MAGIC_0] = RAWHID_APP_MAGIC_0;
    hello_response[RAWHID_APP_OFFSET_MAGIC_1] = RAWHID_APP_MAGIC_1;
    hello_response[RAWHID_APP_OFFSET_VERSION] = RAWHID_APP_VERSION;
    hello_response[RAWHID_APP_OFFSET_TYPE]    = RAWHID_APP_PACKET_DEVICE_HELLO;
    hello_response[RAWHID_APP_OFFSET_SEQ] = seq;
    hello_response[RAWHID_APP_OFFSET_PAYLOAD_LEN] = RAWHID_APP_HELLO_PAYLOAD_LEN;
    uint8_t *payload = &hello_response[RAWHID_APP_OFFSET_PAYLOAD];
    sys_put_le32(rawhid_app_identity_get_capabilities(),
                 &payload[RAWHID_APP_HELLO_CAPABILITIES]);
    sys_put_le64(rawhid_app_identity_get_uid_hash(),
                 &payload[RAWHID_APP_HELLO_DEVICE_UID_HASH]);

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = hello_response,
        .length = sizeof(hello_response),
    });
}

static int rawhid_app_received_listener(const zmk_event_t *eh) {
    struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    struct rawhid_app_packet packet;

    if (!parse_packet(event, &packet)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (packet.type) {
    case RAWHID_APP_PACKET_HOST_HELLO:
        send_device_hello(packet.hello.seq);
        rawhid_app_uplink_schedule_initial_push();
        break;
    case RAWHID_APP_PACKET_APP_LAYER:
#if IS_ENABLED(CONFIG_RAWHID_APP_LAYER_CONTROL)
        rawhid_app_layer_control_handle(&packet);
#endif
        break;
    case RAWHID_APP_PACKET_TIME_SYNC:
#if IS_ENABLED(CONFIG_RAWHID_APP_TIME_SYNC)
        rawhid_app_time_sync_handle(&packet);
#endif
        break;
    case RAWHID_APP_PACKET_AI_USAGE:
#if IS_ENABLED(CONFIG_RAWHID_APP_AI_USAGE)
        rawhid_app_ai_usage_handle(&packet);
#endif
        break;
    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app, rawhid_app_received_listener);
ZMK_SUBSCRIPTION(rawhid_app, raw_hid_received_event);
