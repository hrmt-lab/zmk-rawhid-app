#include <stdint.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LAYER_STATE_ACTIVE_LAYER 4
#define LAYER_STATE_MASK 8
#define LAYER_STATE_DEBOUNCE_MS 50

static void layer_state_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(layer_state_work, layer_state_work_handler);

void rawhid_app_layer_state_report_send_now(void) {
    uint8_t buf[RAWHID_APP_PACKET_SIZE];
    uint8_t active_layer = zmk_keymap_highest_layer_active();
    zmk_keymap_layers_state_t mask = zmk_keymap_layer_state();

    if (mask != 0 && active_layer < 32 && (mask & BIT(active_layer)) == 0) {
        mask |= BIT(active_layer);
    }

    rawhid_app_uplink_prepare(buf, RAWHID_APP_PACKET_LAYER_STATE);
    buf[LAYER_STATE_ACTIVE_LAYER] = active_layer;
    buf[RAWHID_APP_OFFSET_SEQ] = rawhid_app_uplink_next_seq(RAWHID_APP_PACKET_LAYER_STATE);
    sys_put_le32((uint32_t)mask, &buf[LAYER_STATE_MASK]);

    rawhid_app_uplink_send(buf);
}

static void layer_state_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    rawhid_app_layer_state_report_send_now();
}

static int layer_state_listener(const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_work_reschedule(&layer_state_work, K_MSEC(LAYER_STATE_DEBOUNCE_MS));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_layer_state_report, layer_state_listener);
ZMK_SUBSCRIPTION(rawhid_app_layer_state_report, zmk_layer_state_changed);
