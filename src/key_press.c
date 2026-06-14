#include <stdint.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include <rawhid_app/uplink.h>

#define KEY_PRESS_POSITION 4
#define KEY_PRESS_FLAGS 5
#define KEY_PRESS_PRESSED 0x01

static int key_press_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || ev->position > UINT8_MAX) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t buf[RAWHID_APP_PACKET_SIZE];

    rawhid_app_uplink_prepare(buf, RAWHID_APP_PACKET_KEY_PRESS);
    buf[KEY_PRESS_POSITION] = (uint8_t)ev->position;
    buf[KEY_PRESS_FLAGS] = ev->state ? KEY_PRESS_PRESSED : 0x00;
    buf[RAWHID_APP_OFFSET_SEQ] = rawhid_app_uplink_next_seq(RAWHID_APP_PACKET_KEY_PRESS);

    rawhid_app_uplink_send(buf);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_key_press, key_press_listener);
ZMK_SUBSCRIPTION(rawhid_app_key_press, zmk_position_state_changed);
