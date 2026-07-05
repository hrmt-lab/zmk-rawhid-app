#pragma once

#include <stdint.h>

#include <rawhid_app/packet.h>

void rawhid_app_uplink_prepare(uint8_t *buf, enum rawhid_app_packet_type type,
                               uint8_t payload_len);
uint8_t rawhid_app_uplink_next_seq(enum rawhid_app_packet_type type);
int rawhid_app_uplink_send(uint8_t *buf);
void rawhid_app_uplink_schedule_initial_push(void);

void rawhid_app_layer_state_report_send_now(void);
void rawhid_app_battery_report_send_now(void);
int rawhid_app_host_action_send(uint8_t action_id, uint8_t value);
