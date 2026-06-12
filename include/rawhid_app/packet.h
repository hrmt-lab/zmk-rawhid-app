#pragma once

#include <stdint.h>

/* RawHID application protocol (v1).
 *
 * 32 byte reports with a common header:
 *   0..1 magic "HL" / 2 version 0x01 / 3 type / 4..31 payload (reserved = 0).
 * All multi-byte fields are little-endian.
 */

#define RAWHID_APP_PACKET_SIZE 32
#define RAWHID_APP_MAX_LAYER   31

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
};

enum rawhid_app_app_layer_action {
    RAWHID_APP_APP_LAYER_SET = 1,
    RAWHID_APP_APP_LAYER_CLEAR = 2,
};

enum rawhid_app_ai_provider {
    RAWHID_APP_AI_PROVIDER_CODEX = 1,
    RAWHID_APP_AI_PROVIDER_CLAUDE_CODE = 2,
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
    };
};

/* Handlers implemented in the per-feature sources, called by dispatch.c. */
void rawhid_app_layer_control_handle(const struct rawhid_app_packet *packet);
void rawhid_app_time_sync_handle(const struct rawhid_app_packet *packet);
void rawhid_app_ai_usage_handle(const struct rawhid_app_packet *packet);
