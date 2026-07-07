#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <raw_hid/events.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/encoder_runtime.h>
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

/* CONFIG_RPC ENCODER payload offsets. */
#define RAWHID_APP_CONFIG_ENCODER_GET_INFO_PAYLOAD_LEN 4
#define RAWHID_APP_CONFIG_ENCODER_GET_BINDINGS_REQUEST_LEN 5
#define RAWHID_APP_CONFIG_ENCODER_GET_BINDINGS_RESPONSE_LEN 28
#define RAWHID_APP_CONFIG_ENCODER_SET_BINDINGS_REQUEST_LEN 28
#define RAWHID_APP_CONFIG_ENCODER_GET_DIRTY_RESPONSE_LEN 1
#define RAWHID_APP_CONFIG_ENCODER_CLEAR_OVERRIDE_REQUEST_LEN 5
#define RAWHID_APP_CONFIG_ENCODER_LAYER_ID 0
#define RAWHID_APP_CONFIG_ENCODER_ENCODER_ID 4
#define RAWHID_APP_CONFIG_ENCODER_SET_UPDATE_MASK 5
#define RAWHID_APP_CONFIG_ENCODER_SET_UPDATE_MASK_BOTH 0x03
#define RAWHID_APP_CONFIG_ENCODER_SET_RESERVED_0 6
#define RAWHID_APP_CONFIG_ENCODER_SET_RESERVED_1 7
#define RAWHID_APP_CONFIG_ENCODER_BINDINGS_SOURCE 5
#define RAWHID_APP_CONFIG_ENCODER_BINDINGS_FLAGS 6
#define RAWHID_APP_CONFIG_ENCODER_BINDINGS_RESERVED 7
#define RAWHID_APP_CONFIG_ENCODER_CW_BINDING 8
#define RAWHID_APP_CONFIG_ENCODER_CCW_BINDING 18
#define RAWHID_APP_CONFIG_ENCODER_BINDING_LEN 10
#define RAWHID_APP_CONFIG_ENCODER_BINDING_BEHAVIOR_ID 0
#define RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM1 2
#define RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM2 6
#define RAWHID_APP_CONFIG_ENCODER_SOURCE_KEYMAP 0x00
#define RAWHID_APP_CONFIG_ENCODER_SOURCE_OVERRIDE 0x01
#define RAWHID_APP_CONFIG_ENCODER_FLAG_STALE_SAVED_EXISTS BIT(0)
#define RAWHID_APP_CONFIG_ENCODER_FLAG_SAVED_EXISTS BIT(1)
#define RAWHID_APP_CONFIG_ENCODER_FLAG_RUNTIME_DIRTY BIT(2)
#define RAWHID_APP_CONFIG_ENCODER_FLAG_INVALID_SAVED_EXISTS BIT(3)
#define RAWHID_APP_CONFIG_ENCODER_INVALID_BEHAVIOR_ID UINT16_MAX

BUILD_ASSERT(CONFIG_RAW_HID_REPORT_SIZE == RAWHID_APP_PACKET_SIZE,
             "rawhid-app requires 64 byte reports");

static uint8_t hello_response[RAWHID_APP_PACKET_SIZE];
static uint8_t config_response[RAWHID_APP_PACKET_SIZE];

static atomic_t config_encoder_save_pending;
static uint8_t config_encoder_save_seq;
static uint8_t config_encoder_save_feature;
static uint8_t config_encoder_save_op;

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

static bool parse_config_request_packet(const uint8_t *data, struct rawhid_app_packet *packet,
                                        uint8_t payload_len) {
    const uint8_t *payload = &data[RAWHID_APP_OFFSET_PAYLOAD];

    packet->config_request.seq = data[RAWHID_APP_OFFSET_SEQ];
    packet->config_request.feature = data[RAWHID_APP_OFFSET_FEATURE];
    packet->config_request.op = data[RAWHID_APP_OFFSET_OP];
    packet->config_request.flags = data[RAWHID_APP_OFFSET_STATUS_OR_FLAGS];
    packet->config_request.payload_len = payload_len;
    if (payload_len > 0) {
        memcpy(packet->config_request.payload, payload, payload_len);
    }
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
        return parse_config_request_packet(data, packet, payload_len);
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

static void send_config_response(uint8_t seq, uint8_t feature, uint8_t op,
                                 enum rawhid_app_config_status status, const uint8_t *payload,
                                 uint8_t payload_len) {
    memset(config_response, 0, sizeof(config_response));
    config_response[RAWHID_APP_OFFSET_MAGIC_0] = RAWHID_APP_MAGIC_0;
    config_response[RAWHID_APP_OFFSET_MAGIC_1] = RAWHID_APP_MAGIC_1;
    config_response[RAWHID_APP_OFFSET_VERSION] = RAWHID_APP_VERSION;
    config_response[RAWHID_APP_OFFSET_TYPE] = RAWHID_APP_PACKET_CONFIG_RESPONSE;
    config_response[RAWHID_APP_OFFSET_SEQ] = seq;
    config_response[RAWHID_APP_OFFSET_FEATURE] = feature;
    config_response[RAWHID_APP_OFFSET_OP] = op;
    config_response[RAWHID_APP_OFFSET_STATUS_OR_FLAGS] = status;
    config_response[RAWHID_APP_OFFSET_PAYLOAD_LEN] = payload_len;
    if (payload != NULL && payload_len > 0) {
        memcpy(&config_response[RAWHID_APP_OFFSET_PAYLOAD], payload, payload_len);
    }

    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = config_response,
        .length = sizeof(config_response),
    });
}

static bool config_encoder_decode_binding(const uint8_t *data,
                                          struct zmk_behavior_binding *binding) {
    uint16_t behavior_id = sys_get_le16(&data[RAWHID_APP_CONFIG_ENCODER_BINDING_BEHAVIOR_ID]);
    if (behavior_id == RAWHID_APP_CONFIG_ENCODER_INVALID_BEHAVIOR_ID) {
        return false;
    }

    const char *behavior_dev = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (behavior_dev == NULL) {
        return false;
    }

    *binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_dev,
        .param1 = sys_get_le32(&data[RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM1]),
        .param2 = sys_get_le32(&data[RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM2]),
    };
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    binding->local_id = behavior_id;
#endif

    return zmk_behavior_validate_binding(binding) == 0;
}

static bool config_encoder_encode_binding(const struct zmk_behavior_binding *binding,
                                          uint8_t *data) {
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
    if (behavior_id == RAWHID_APP_CONFIG_ENCODER_INVALID_BEHAVIOR_ID) {
        return false;
    }

    sys_put_le16(behavior_id, &data[RAWHID_APP_CONFIG_ENCODER_BINDING_BEHAVIOR_ID]);
    sys_put_le32(binding->param1, &data[RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM1]);
    sys_put_le32(binding->param2, &data[RAWHID_APP_CONFIG_ENCODER_BINDING_PARAM2]);
    return true;
}

static void handle_config_encoder_get_info(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    uint8_t payload[RAWHID_APP_CONFIG_ENCODER_GET_INFO_PAYLOAD_LEN] = {
        ZMK_KEYMAP_LAYERS_LEN,
        ZMK_KEYMAP_SENSORS_LEN,
        0,
        0,
    };
    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, payload, sizeof(payload));
}

static void handle_config_encoder_get_bindings(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len !=
        RAWHID_APP_CONFIG_ENCODER_GET_BINDINGS_REQUEST_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    const uint8_t *request = packet->config_request.payload;
    uint32_t layer_id = sys_get_le32(&request[RAWHID_APP_CONFIG_ENCODER_LAYER_ID]);
    uint8_t encoder_id = request[RAWHID_APP_CONFIG_ENCODER_ENCODER_ID];

    if (!rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INVALID_ARGUMENT, NULL, 0);
        return;
    }

    uint8_t response[RAWHID_APP_CONFIG_ENCODER_GET_BINDINGS_RESPONSE_LEN] = {0};
    sys_put_le32(layer_id, &response[RAWHID_APP_CONFIG_ENCODER_LAYER_ID]);
    response[RAWHID_APP_CONFIG_ENCODER_ENCODER_ID] = encoder_id;
    response[RAWHID_APP_CONFIG_ENCODER_BINDINGS_RESERVED] = 0;

    struct rawhid_app_encoder_runtime_bindings bindings;
    if (rawhid_app_encoder_runtime_get(layer_id, encoder_id, &bindings)) {
        response[RAWHID_APP_CONFIG_ENCODER_BINDINGS_SOURCE] = bindings.source;
        response[RAWHID_APP_CONFIG_ENCODER_BINDINGS_FLAGS] = bindings.flags;

        if (bindings.source == RAWHID_APP_CONFIG_ENCODER_SOURCE_OVERRIDE) {
            if (!config_encoder_encode_binding(&bindings.cw_binding,
                                               &response[RAWHID_APP_CONFIG_ENCODER_CW_BINDING]) ||
                !config_encoder_encode_binding(&bindings.ccw_binding,
                                               &response[RAWHID_APP_CONFIG_ENCODER_CCW_BINDING])) {
                send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INTERNAL_ERROR,
                                     NULL, 0);
                return;
            }
        }
    }

    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, response,
                         sizeof(response));
}

static void handle_config_encoder_set_bindings(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len !=
        RAWHID_APP_CONFIG_ENCODER_SET_BINDINGS_REQUEST_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    const uint8_t *request = packet->config_request.payload;
    if (request[RAWHID_APP_CONFIG_ENCODER_SET_RESERVED_0] != 0 ||
        request[RAWHID_APP_CONFIG_ENCODER_SET_RESERVED_1] != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    uint32_t layer_id = sys_get_le32(&request[RAWHID_APP_CONFIG_ENCODER_LAYER_ID]);
    uint8_t encoder_id = request[RAWHID_APP_CONFIG_ENCODER_ENCODER_ID];
    uint8_t update_mask = request[RAWHID_APP_CONFIG_ENCODER_SET_UPDATE_MASK];

    if (update_mask != RAWHID_APP_CONFIG_ENCODER_SET_UPDATE_MASK_BOTH ||
        !rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INVALID_ARGUMENT, NULL, 0);
        return;
    }

    struct zmk_behavior_binding cw_binding;
    struct zmk_behavior_binding ccw_binding;
    if (!config_encoder_decode_binding(&request[RAWHID_APP_CONFIG_ENCODER_CW_BINDING],
                                       &cw_binding) ||
        !config_encoder_decode_binding(&request[RAWHID_APP_CONFIG_ENCODER_CCW_BINDING],
                                       &ccw_binding)) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INVALID_ARGUMENT, NULL, 0);
        return;
    }

    rawhid_app_encoder_runtime_set(layer_id, encoder_id, &cw_binding, &ccw_binding);
    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_config_encoder_get_dirty(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    uint8_t payload[RAWHID_APP_CONFIG_ENCODER_GET_DIRTY_RESPONSE_LEN] = {
        rawhid_app_encoder_runtime_dirty() ? 1 : 0,
    };
    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, payload, sizeof(payload));
}

static void config_encoder_save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    int rc = rawhid_app_encoder_runtime_save();
    send_config_response(config_encoder_save_seq, config_encoder_save_feature,
                         config_encoder_save_op,
                         rc < 0 ? RAWHID_APP_CONFIG_STATUS_STORAGE_ERROR
                                : RAWHID_APP_CONFIG_STATUS_OK,
                         NULL, 0);
    atomic_clear(&config_encoder_save_pending);
}

K_WORK_DEFINE(config_encoder_save_work, config_encoder_save_work_handler);

static void handle_config_encoder_save(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    if (!atomic_cas(&config_encoder_save_pending, 0, 1)) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INTERNAL_ERROR, NULL, 0);
        return;
    }

    config_encoder_save_seq = seq;
    config_encoder_save_feature = feature;
    config_encoder_save_op = op;
    k_work_submit(&config_encoder_save_work);
}

static void handle_config_encoder_discard(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    rawhid_app_encoder_runtime_discard();
    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_config_encoder_clear_override(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.payload_len !=
        RAWHID_APP_CONFIG_ENCODER_CLEAR_OVERRIDE_REQUEST_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    const uint8_t *request = packet->config_request.payload;
    uint32_t layer_id = sys_get_le32(&request[RAWHID_APP_CONFIG_ENCODER_LAYER_ID]);
    uint8_t encoder_id = request[RAWHID_APP_CONFIG_ENCODER_ENCODER_ID];

    if (!rawhid_app_encoder_runtime_layer_exists(layer_id) ||
        encoder_id >= ZMK_KEYMAP_SENSORS_LEN) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_INVALID_ARGUMENT, NULL, 0);
        return;
    }

    rawhid_app_encoder_runtime_clear(layer_id, encoder_id);
    send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_config_request(const struct rawhid_app_packet *packet) {
    uint8_t seq = packet->config_request.seq;
    uint8_t feature = packet->config_request.feature;
    uint8_t op = packet->config_request.op;

    if (packet->config_request.flags != 0) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_BAD_PACKET, NULL, 0);
        return;
    }

    if (feature != RAWHID_APP_CONFIG_FEATURE_ENCODER) {
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_UNSUPPORTED_FEATURE, NULL,
                             0);
        return;
    }

    switch (op) {
    case RAWHID_APP_CONFIG_OP_GET_INFO:
        handle_config_encoder_get_info(packet);
        break;
    case RAWHID_APP_CONFIG_OP_GET_BINDINGS:
        handle_config_encoder_get_bindings(packet);
        break;
    case RAWHID_APP_CONFIG_OP_SET_BINDINGS:
        handle_config_encoder_set_bindings(packet);
        break;
    case RAWHID_APP_CONFIG_OP_GET_DIRTY:
        handle_config_encoder_get_dirty(packet);
        break;
    case RAWHID_APP_CONFIG_OP_SAVE:
        handle_config_encoder_save(packet);
        break;
    case RAWHID_APP_CONFIG_OP_DISCARD:
        handle_config_encoder_discard(packet);
        break;
    case RAWHID_APP_CONFIG_OP_CLEAR_OVERRIDE:
        handle_config_encoder_clear_override(packet);
        break;
    default:
        send_config_response(seq, feature, op, RAWHID_APP_CONFIG_STATUS_UNSUPPORTED_OP, NULL, 0);
        break;
    }
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
    case RAWHID_APP_PACKET_CONFIG_REQUEST:
#if IS_ENABLED(CONFIG_RAWHID_APP_CONFIG_RPC)
        handle_config_request(&packet);
#endif
        break;
    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app, rawhid_app_received_listener);
ZMK_SUBSCRIPTION(rawhid_app, raw_hid_received_event);
