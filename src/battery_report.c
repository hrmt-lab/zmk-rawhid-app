#include <stdbool.h>
#include <errno.h>
#include <stdint.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#if __has_include(<zmk/events/split_central_status_changed.h>)
#define RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS 1
#include <zmk/events/split_central_status_changed.h>
#else
#define RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS 0
#endif

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_STATUS_COUNT 4
#define BATTERY_STATUS_ENTRIES 5
#define BATTERY_REPORT_INTERVAL_MIN 5
#define BATTERY_SOURCE_COUNT 2
#define BATTERY_LEVEL_DISCONNECTED 0xff

static uint8_t levels[BATTERY_SOURCE_COUNT] = {BATTERY_LEVEL_DISCONNECTED,
                                               BATTERY_LEVEL_DISCONNECTED};
#if RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS
static bool connected[BATTERY_SOURCE_COUNT] = {false, false};
#endif

static void battery_periodic_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_periodic_work, battery_periodic_work_handler);

static int send_entries(const uint8_t *sources, const uint8_t *entry_levels, uint8_t count) {
    if (count == 0 || count > 4) {
        return -EINVAL;
    }

    uint8_t buf[RAWHID_APP_PACKET_SIZE];

    rawhid_app_uplink_prepare(buf, RAWHID_APP_PACKET_BATTERY_STATUS);
    buf[BATTERY_STATUS_COUNT] = count;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t offset = BATTERY_STATUS_ENTRIES + (i * 2);
        buf[offset] = sources[i];
        buf[offset + 1] = entry_levels[i];
    }

    return rawhid_app_uplink_send(buf);
}

static void send_source(uint8_t source_index) {
    uint8_t source = source_index + 1;
    uint8_t level = levels[source_index];

    send_entries(&source, &level, 1);
}

void rawhid_app_battery_report_send_now(void) {
    uint8_t sources[BATTERY_SOURCE_COUNT];
    uint8_t entry_levels[BATTERY_SOURCE_COUNT];

    for (uint8_t i = 0; i < BATTERY_SOURCE_COUNT; i++) {
        sources[i] = i + 1;
        entry_levels[i] = levels[i];
    }

    send_entries(sources, entry_levels, BATTERY_SOURCE_COUNT);
}

static void schedule_periodic(void) {
    k_work_reschedule(&battery_periodic_work, K_MINUTES(BATTERY_REPORT_INTERVAL_MIN));
}

static void battery_periodic_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    rawhid_app_battery_report_send_now();
    schedule_periodic();
}

static int battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev == NULL || ev->source >= BATTERY_SOURCE_COUNT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t level = MIN(ev->state_of_charge, 100);
#if RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS
    if (!connected[ev->source] && level == 0) {
        level = BATTERY_LEVEL_DISCONNECTED;
    } else if (level > 0) {
        connected[ev->source] = true;
    }
#endif

    if (levels[ev->source] != level) {
        levels[ev->source] = level;
        send_source(ev->source);
    }

    schedule_periodic();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_battery_report, battery_listener);
ZMK_SUBSCRIPTION(rawhid_app_battery_report, zmk_peripheral_battery_state_changed);

#if RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS
static int battery_connection_listener(const zmk_event_t *eh) {
    const struct zmk_split_central_status_changed *ev = as_zmk_split_central_status_changed(eh);

    if (ev == NULL || ev->slot >= BATTERY_SOURCE_COUNT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    connected[ev->slot] = ev->connected;

    if (!ev->connected && levels[ev->slot] != BATTERY_LEVEL_DISCONNECTED) {
        levels[ev->slot] = BATTERY_LEVEL_DISCONNECTED;
        send_source(ev->slot);
    }

    schedule_periodic();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_battery_connection_report, battery_connection_listener);
ZMK_SUBSCRIPTION(rawhid_app_battery_connection_report, zmk_split_central_status_changed);
#endif
