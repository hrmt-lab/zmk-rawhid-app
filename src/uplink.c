#include <stdint.h>
#include <string.h>

#include <raw_hid/events.h>

#include <zmk/event_manager.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define INITIAL_PUSH_DELAY_MS 150

static uint8_t host_action_seq;
static uint8_t key_stats_seq;
static uint8_t layer_state_seq;

static void initial_push_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(initial_push_work, initial_push_work_handler);

void rawhid_app_uplink_prepare(uint8_t *buf, enum rawhid_app_packet_type type) {
    memset(buf, 0, RAWHID_APP_PACKET_SIZE);
    buf[RAWHID_APP_OFFSET_MAGIC_0] = RAWHID_APP_MAGIC_0;
    buf[RAWHID_APP_OFFSET_MAGIC_1] = RAWHID_APP_MAGIC_1;
    buf[RAWHID_APP_OFFSET_VERSION] = RAWHID_APP_VERSION;
    buf[RAWHID_APP_OFFSET_TYPE] = type;
}

uint8_t rawhid_app_uplink_next_seq(enum rawhid_app_packet_type type) {
    switch (type) {
    case RAWHID_APP_PACKET_HOST_ACTION:
        return host_action_seq++;
    case RAWHID_APP_PACKET_KEY_STATS:
        return key_stats_seq++;
    case RAWHID_APP_PACKET_LAYER_STATE:
        return layer_state_seq++;
    default:
        return 0;
    }
}

int rawhid_app_uplink_send(uint8_t *buf) {
    int err = raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = buf,
        .length = RAWHID_APP_PACKET_SIZE,
    });

    if (err < 0) {
        LOG_WRN("Failed to raise RawHID uplink packet 0x%02x: %d", buf[RAWHID_APP_OFFSET_TYPE],
                err);
    }

    return err;
}

static void initial_push_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_RAWHID_APP_LAYER_STATE_REPORT)
    rawhid_app_layer_state_report_send_now();
#endif

#if IS_ENABLED(CONFIG_RAWHID_APP_BATTERY_REPORT)
    rawhid_app_battery_report_send_now();
#endif
}

void rawhid_app_uplink_schedule_initial_push(void) {
    k_work_reschedule(&initial_push_work, K_MSEC(INITIAL_PUSH_DELAY_MS));
}
