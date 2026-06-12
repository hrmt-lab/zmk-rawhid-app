#pragma once

#include <stdint.h>

#include <rawhid_app/packet.h>

#define RAWHID_APP_OFFSET_MAGIC_0 0
#define RAWHID_APP_OFFSET_MAGIC_1 1
#define RAWHID_APP_OFFSET_VERSION 2
#define RAWHID_APP_OFFSET_TYPE 3
#define RAWHID_APP_OFFSET_SEQ 7

#define RAWHID_APP_MAGIC_0 'H'
#define RAWHID_APP_MAGIC_1 'L'
#define RAWHID_APP_VERSION 0x01

void rawhid_app_uplink_prepare(uint8_t *buf, enum rawhid_app_packet_type type);
uint8_t rawhid_app_uplink_next_seq(enum rawhid_app_packet_type type);
int rawhid_app_uplink_send(uint8_t *buf);
void rawhid_app_uplink_schedule_initial_push(void);

void rawhid_app_layer_state_report_send_now(void);
void rawhid_app_battery_report_send_now(void);
int rawhid_app_host_action_send(uint8_t action_id, uint8_t value);
