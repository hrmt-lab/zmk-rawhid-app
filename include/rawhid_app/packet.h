#pragma once

#include <stdint.h>

/* RawHID application protocol (v2).
 *
 * 64 byte packets with a common header:
 *   0..1 magic "HL" / 2 version 0x02 / 3 type / 4 seq /
 *   5 feature / 6 op / 7 status_or_flags / 8 payload_len /
 *   9..11 reserved / 12..63 payload.
 * All multi-byte fields are little-endian.
 */

#define RAWHID_APP_PACKET_SIZE 64
#define RAWHID_APP_PAYLOAD_SIZE 52
#define RAWHID_APP_MAX_LAYER   31

#define RAWHID_APP_OFFSET_MAGIC_0 0
#define RAWHID_APP_OFFSET_MAGIC_1 1
#define RAWHID_APP_OFFSET_VERSION 2
#define RAWHID_APP_OFFSET_TYPE 3
#define RAWHID_APP_OFFSET_SEQ 4
#define RAWHID_APP_OFFSET_FEATURE 5
#define RAWHID_APP_OFFSET_OP 6
#define RAWHID_APP_OFFSET_STATUS_OR_FLAGS 7
#define RAWHID_APP_OFFSET_PAYLOAD_LEN 8
#define RAWHID_APP_OFFSET_RESERVED_START 9
#define RAWHID_APP_OFFSET_RESERVED_END 11
#define RAWHID_APP_OFFSET_PAYLOAD 12

#define RAWHID_APP_MAGIC_0 'H'
#define RAWHID_APP_MAGIC_1 'L'
#define RAWHID_APP_VERSION 0x02

enum rawhid_app_packet_type {
    RAWHID_APP_PACKET_HOST_HELLO = 0x01,
    RAWHID_APP_PACKET_DEVICE_HELLO = 0x02,
    RAWHID_APP_PACKET_ERROR = 0x03,
    RAWHID_APP_PACKET_PING = 0x04,
    RAWHID_APP_PACKET_PONG = 0x05,
    RAWHID_APP_PACKET_AI_USAGE = 0x10,
    RAWHID_APP_PACKET_TIME_SYNC = 0x20,
    RAWHID_APP_PACKET_APP_LAYER = 0x30,
    RAWHID_APP_PACKET_BATTERY_STATUS = 0x40,
    RAWHID_APP_PACKET_HOST_ACTION = 0x50,
    RAWHID_APP_PACKET_KEY_STATS = 0x60,
    RAWHID_APP_PACKET_LAYER_STATE = 0x70,
    RAWHID_APP_PACKET_KEY_PRESS = 0x80,
    RAWHID_APP_PACKET_CONFIG_REQUEST = 0x90,
    RAWHID_APP_PACKET_CONFIG_RESPONSE = 0x91,
};

enum rawhid_app_app_layer_action {
    RAWHID_APP_APP_LAYER_SET = 1,
    RAWHID_APP_APP_LAYER_CLEAR = 2,
};

enum rawhid_app_ai_provider {
    RAWHID_APP_AI_PROVIDER_CODEX = 1,
    RAWHID_APP_AI_PROVIDER_CLAUDE_CODE = 2,
};

enum rawhid_app_config_feature {
    RAWHID_APP_CONFIG_FEATURE_ENCODER = 0x01,
    RAWHID_APP_CONFIG_FEATURE_COMBO = 0x02,
};

enum rawhid_app_config_op {
    RAWHID_APP_CONFIG_OP_GET_INFO = 0x01,
    RAWHID_APP_CONFIG_OP_GET_BINDINGS = 0x02,
    RAWHID_APP_CONFIG_OP_SET_BINDINGS = 0x03,
    RAWHID_APP_CONFIG_OP_GET_DIRTY = 0x04,
    RAWHID_APP_CONFIG_OP_SAVE = 0x05,
    RAWHID_APP_CONFIG_OP_DISCARD = 0x06,
    RAWHID_APP_CONFIG_OP_CLEAR_OVERRIDE = 0x07,
};

enum rawhid_app_config_combo_op {
    RAWHID_APP_CONFIG_COMBO_OP_GET_INFO = 0x01,
    RAWHID_APP_CONFIG_COMBO_OP_GET_COMBO = 0x02,
    RAWHID_APP_CONFIG_COMBO_OP_SET_COMBO = 0x03,
    RAWHID_APP_CONFIG_COMBO_OP_GET_DIRTY = 0x04,
    RAWHID_APP_CONFIG_COMBO_OP_SAVE = 0x05,
    RAWHID_APP_CONFIG_COMBO_OP_DISCARD = 0x06,
    RAWHID_APP_CONFIG_COMBO_OP_DELETE_COMBO = 0x07,
    RAWHID_APP_CONFIG_COMBO_OP_RESET_TO_KEYMAP = 0x08,
};

enum rawhid_app_config_status {
    RAWHID_APP_CONFIG_STATUS_OK = 0x00,
    RAWHID_APP_CONFIG_STATUS_BAD_PACKET = 0x01,
    RAWHID_APP_CONFIG_STATUS_UNSUPPORTED_FEATURE = 0x02,
    RAWHID_APP_CONFIG_STATUS_UNSUPPORTED_OP = 0x03,
    RAWHID_APP_CONFIG_STATUS_INVALID_ARGUMENT = 0x04,
    RAWHID_APP_CONFIG_STATUS_BUSY = 0x05,
    RAWHID_APP_CONFIG_STATUS_NOT_FOUND = 0x06,
    RAWHID_APP_CONFIG_STATUS_STORAGE_ERROR = 0x07,
    RAWHID_APP_CONFIG_STATUS_INTERNAL_ERROR = 0x08,
};

enum rawhid_app_time_format {
    RAWHID_APP_TIME_FORMAT_TIME_HM = 0,
    RAWHID_APP_TIME_FORMAT_TIME_HMS = 1,
    RAWHID_APP_TIME_FORMAT_DATE_YMD = 2,
    RAWHID_APP_TIME_FORMAT_DATE_MD = 3,
    RAWHID_APP_TIME_FORMAT_DATETIME_HM = 4,
    RAWHID_APP_TIME_FORMAT_WEEKDAY_HM = 5,
};

enum rawhid_app_clock_mode {
    RAWHID_APP_CLOCK_24H = 0,
    RAWHID_APP_CLOCK_12H = 1,
};

struct rawhid_app_packet {
    enum rawhid_app_packet_type type;
    union {
        struct {
            uint8_t seq;
        } hello;
        struct {
            uint8_t action;
            uint8_t layer;
            uint8_t seq;
        } app_layer;
        struct {
            uint32_t unix_time_sec;
            int16_t tz_offset_min;
            uint8_t weekday;
            enum rawhid_app_time_format format_hint;
            enum rawhid_app_clock_mode clock_mode;
        } time_sync;
        struct {
            uint8_t provider;
            uint8_t flags;
            uint16_t five_hour_used_bp;
            uint16_t seven_day_used_bp;
            uint32_t five_hour_reset_unix;
            uint32_t seven_day_reset_unix;
            uint32_t updated_unix;
            uint8_t error_code;
        } ai_usage;
        struct {
            uint8_t seq;
            uint8_t feature;
            uint8_t op;
            uint8_t flags;
            uint8_t payload_len;
            uint8_t payload[RAWHID_APP_PAYLOAD_SIZE];
        } config_request;
    };
};

/* Handlers implemented in the per-feature sources, called by dispatch.c. */
void rawhid_app_layer_control_handle(const struct rawhid_app_packet *packet);
void rawhid_app_time_sync_handle(const struct rawhid_app_packet *packet);
void rawhid_app_ai_usage_handle(const struct rawhid_app_packet *packet);
