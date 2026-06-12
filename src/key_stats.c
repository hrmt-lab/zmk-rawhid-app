#include <stdbool.h>
#include <stdint.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KEY_STATS_COUNT 4
#define KEY_STATS_FLAGS 5
#define KEY_STATS_ENTRIES 8
#define KEY_STATS_ENTRY_SIZE 3
#define KEY_STATS_MAX_ENTRIES 8
#define KEY_STATS_MORE_FOLLOWS BIT(0)
#define KEY_STATS_INTERVAL_SEC 45

static uint16_t counters[ZMK_KEYMAP_LEN];

static void key_stats_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(key_stats_work, key_stats_work_handler);

static void schedule_next_flush(void) {
    k_work_reschedule(&key_stats_work, K_SECONDS(KEY_STATS_INTERVAL_SEC));
}

static void send_packet(uint8_t positions[KEY_STATS_MAX_ENTRIES],
                        uint16_t deltas[KEY_STATS_MAX_ENTRIES], uint8_t count,
                        bool more_follows) {
    uint8_t buf[RAWHID_APP_PACKET_SIZE];

    rawhid_app_uplink_prepare(buf, RAWHID_APP_PACKET_KEY_STATS);
    buf[KEY_STATS_COUNT] = count;
    buf[KEY_STATS_FLAGS] = more_follows ? KEY_STATS_MORE_FOLLOWS : 0;
    buf[RAWHID_APP_OFFSET_SEQ] = rawhid_app_uplink_next_seq(RAWHID_APP_PACKET_KEY_STATS);

    for (uint8_t i = 0; i < count; i++) {
        uint8_t offset = KEY_STATS_ENTRIES + (i * KEY_STATS_ENTRY_SIZE);
        buf[offset] = positions[i];
        sys_put_le16(deltas[i], &buf[offset + 1]);
    }

    rawhid_app_uplink_send(buf);
}

static void key_stats_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t positions[KEY_STATS_MAX_ENTRIES];
    uint16_t deltas[KEY_STATS_MAX_ENTRIES];
    uint8_t count = 0;

    for (uint16_t position = 0; position < ZMK_KEYMAP_LEN && position <= UINT8_MAX; position++) {
        uint16_t delta = counters[position];

        if (delta == 0) {
            continue;
        }

        counters[position] = 0;
        positions[count] = (uint8_t)position;
        deltas[count] = delta;
        count++;

        if (count == KEY_STATS_MAX_ENTRIES) {
            bool more = false;

            for (uint16_t next = position + 1; next < ZMK_KEYMAP_LEN && next <= UINT8_MAX;
                 next++) {
                if (counters[next] != 0) {
                    more = true;
                    break;
                }
            }

            send_packet(positions, deltas, count, more);
            count = 0;
        }
    }

    if (count > 0) {
        send_packet(positions, deltas, count, false);
    }

    schedule_next_flush();
}

static int key_stats_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL || !ev->state || ev->position >= ZMK_KEYMAP_LEN ||
        ev->position > UINT8_MAX) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint16_t *counter = &counters[ev->position];

    if (*counter < UINT16_MAX) {
        (*counter)++;
    }

    schedule_next_flush();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_key_stats, key_stats_listener);
ZMK_SUBSCRIPTION(rawhid_app_key_stats, zmk_position_state_changed);
