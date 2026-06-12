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

/* HOST_HELLO (incoming): reserved 4..6, seq at 7, reserved 8..31. */
#define RAWHID_APP_HELLO_RESERVED_A_START 4
#define RAWHID_APP_HELLO_RESERVED_A_END 6
#define RAWHID_APP_HELLO_SEQ 7
#define RAWHID_APP_HELLO_RESERVED_B_START 8

/* DEVICE_HELLO (outgoing) extension fields. */
#define RAWHID_APP_HELLO_PROTOCOL_MIN    4  /* u8 */
#define RAWHID_APP_HELLO_PROTOCOL_MAX    5  /* u8 */
/* byte 6: reserved = 0 */
/* byte 7: seq (RAWHID_APP_HELLO_SEQ) */
#define RAWHID_APP_HELLO_CAPABILITIES   8   /* u32 LE */
#define RAWHID_APP_HELLO_DEVICE_UID_HASH 12 /* u64 LE */
/* bytes 20..31: reserved = 0 */

/* APP_LAYER: action at 4, layer at 5, reserved 6, seq 7, reserved 8..31. */
#define RAWHID_APP_APP_LAYER_ACTION 4
#define RAWHID_APP_APP_LAYER_LAYER 5
#define RAWHID_APP_APP_LAYER_RESERVED 6
#define RAWHID_APP_APP_LAYER_SEQ 7
#define RAWHID_APP_APP_LAYER_RESERVED_B_START 8

/* TIME_SYNC payload offsets. */
#define RAWHID_APP_TIME_SYNC_UNIX_TIME_SEC 4
#define RAWHID_APP_TIME_SYNC_TZ_OFFSET_MIN 8
#define RAWHID_APP_TIME_SYNC_WEEKDAY 10
#define RAWHID_APP_TIME_SYNC_FORMAT_HINT 11
#define RAWHID_APP_TIME_SYNC_CLOCK_MODE 12
#define RAWHID_APP_TIME_SYNC_RESERVED_START 13

/* AI_USAGE payload offsets. */
#define RAWHID_APP_AI_PROVIDER 4
#define RAWHID_APP_AI_FLAGS 5
#define RAWHID_APP_AI_FIVE_HOUR_USED_BP 6
#define RAWHID_APP_AI_SEVEN_DAY_USED_BP 8
#define RAWHID_APP_AI_FIVE_HOUR_RESET 10
#define RAWHID_APP_AI_SEVEN_DAY_RESET 14
#define RAWHID_APP_AI_UPDATED 18
#define RAWHID_APP_AI_ERROR_CODE 22
#define RAWHID_APP_AI_RESERVED_START 23

BUILD_ASSERT(CONFIG_RAW_HID_REPORT_SIZE == RAWHID_APP_PACKET_SIZE,
             "rawhid-app requires 32 byte reports");

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
        return true;
    default:
        return false;
    }
}

static bool parse_hello_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    if (!reserved_bytes_are_zero(data, RAWHID_APP_HELLO_RESERVED_A_START,
                                 RAWHID_APP_HELLO_RESERVED_A_END) ||
        !reserved_bytes_are_zero(data, RAWHID_APP_HELLO_RESERVED_B_START,
                                 RAWHID_APP_PACKET_SIZE - 1)) {
        return false;
    }

    packet->hello.seq = data[RAWHID_APP_HELLO_SEQ];
    return true;
}

static bool parse_app_layer_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    uint8_t action = data[RAWHID_APP_APP_LAYER_ACTION];
    uint8_t layer = data[RAWHID_APP_APP_LAYER_LAYER];

    if (action != RAWHID_APP_APP_LAYER_SET && action != RAWHID_APP_APP_LAYER_CLEAR) {
        return false;
    }

    if (action == RAWHID_APP_APP_LAYER_SET && layer > RAWHID_APP_MAX_LAYER) {
        return false;
    }

    if (data[RAWHID_APP_APP_LAYER_RESERVED] != 0 ||
        !reserved_bytes_are_zero(data, RAWHID_APP_APP_LAYER_RESERVED_B_START,
                                 RAWHID_APP_PACKET_SIZE - 1)) {
        return false;
    }

    packet->app_layer.action = action;
    packet->app_layer.layer = layer;
    packet->app_layer.seq = data[RAWHID_APP_APP_LAYER_SEQ];
    return true;
}

static bool parse_time_sync_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    uint8_t weekday = data[RAWHID_APP_TIME_SYNC_WEEKDAY];
    uint8_t format_hint = data[RAWHID_APP_TIME_SYNC_FORMAT_HINT];
    uint8_t clock_mode = data[RAWHID_APP_TIME_SYNC_CLOCK_MODE];

    if (weekday < 1 || weekday > 7) {
        return false;
    }

    if (!reserved_bytes_are_zero(data, RAWHID_APP_TIME_SYNC_RESERVED_START,
                                 RAWHID_APP_PACKET_SIZE - 1)) {
        return false;
    }

    if (format_hint > RAWHID_APP_TIME_FORMAT_WEEKDAY_HM) {
        format_hint = RAWHID_APP_TIME_FORMAT_TIME_HM;
    }

    if (clock_mode > RAWHID_APP_CLOCK_12H) {
        clock_mode = RAWHID_APP_CLOCK_24H;
    }

    packet->time_sync.unix_time_sec = sys_get_le32(&data[RAWHID_APP_TIME_SYNC_UNIX_TIME_SEC]);
    packet->time_sync.tz_offset_min =
        (int16_t)sys_get_le16(&data[RAWHID_APP_TIME_SYNC_TZ_OFFSET_MIN]);
    packet->time_sync.weekday = weekday;
    packet->time_sync.format_hint = (enum rawhid_app_time_format)format_hint;
    packet->time_sync.clock_mode = (enum rawhid_app_clock_mode)clock_mode;
    return true;
}

static bool parse_ai_usage_packet(const uint8_t *data, struct rawhid_app_packet *packet) {
    uint8_t provider = data[RAWHID_APP_AI_PROVIDER];

    if (provider != RAWHID_APP_AI_PROVIDER_CODEX &&
        provider != RAWHID_APP_AI_PROVIDER_CLAUDE_CODE) {
        return false;
    }

    if (!reserved_bytes_are_zero(data, RAWHID_APP_AI_RESERVED_START, RAWHID_APP_PACKET_SIZE - 1)) {
        return false;
    }

    uint16_t five_hour_bp = sys_get_le16(&data[RAWHID_APP_AI_FIVE_HOUR_USED_BP]);
    uint16_t seven_day_bp = sys_get_le16(&data[RAWHID_APP_AI_SEVEN_DAY_USED_BP]);

    packet->ai_usage.provider = provider;
    packet->ai_usage.flags = data[RAWHID_APP_AI_FLAGS];
    packet->ai_usage.five_hour_used_bp = MIN(five_hour_bp, 10000);
    packet->ai_usage.seven_day_used_bp = MIN(seven_day_bp, 10000);
    packet->ai_usage.five_hour_reset_unix = sys_get_le32(&data[RAWHID_APP_AI_FIVE_HOUR_RESET]);
    packet->ai_usage.seven_day_reset_unix = sys_get_le32(&data[RAWHID_APP_AI_SEVEN_DAY_RESET]);
    packet->ai_usage.updated_unix = sys_get_le32(&data[RAWHID_APP_AI_UPDATED]);
    packet->ai_usage.error_code = data[RAWHID_APP_AI_ERROR_CODE];
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

    packet->type = data[RAWHID_APP_OFFSET_TYPE];

    switch (packet->type) {
    case RAWHID_APP_PACKET_HOST_HELLO:
    case RAWHID_APP_PACKET_DEVICE_HELLO:
        return parse_hello_packet(data, packet);
    case RAWHID_APP_PACKET_APP_LAYER:
        return parse_app_layer_packet(data, packet);
    case RAWHID_APP_PACKET_TIME_SYNC:
        return parse_time_sync_packet(data, packet);
    case RAWHID_APP_PACKET_AI_USAGE:
        return parse_ai_usage_packet(data, packet);
    /* ERROR / PING / PONG are reserved in v1: accepted but not acted upon. */
    case RAWHID_APP_PACKET_ERROR:
    case RAWHID_APP_PACKET_PING:
    case RAWHID_APP_PACKET_PONG:
        return false;
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
    hello_response[RAWHID_APP_HELLO_PROTOCOL_MIN] = 0x01;
    hello_response[RAWHID_APP_HELLO_PROTOCOL_MAX] = 0x01;
    /* byte 6: reserved = 0 (covered by memset) */
    hello_response[RAWHID_APP_HELLO_SEQ] = seq;
    sys_put_le32(rawhid_app_identity_get_capabilities(),
                 &hello_response[RAWHID_APP_HELLO_CAPABILITIES]);
    sys_put_le64(rawhid_app_identity_get_uid_hash(),
                 &hello_response[RAWHID_APP_HELLO_DEVICE_UID_HASH]);
    /* bytes 20..31: reserved = 0 (covered by memset) */

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
