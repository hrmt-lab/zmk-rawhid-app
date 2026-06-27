#include <stdbool.h>
#include <errno.h>
#include <stdint.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#define BATTERY_PERIPHERAL_SOURCE_COUNT MIN(ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT, 3)
#else
#define BATTERY_PERIPHERAL_SOURCE_COUNT 0
#endif

#if __has_include(<zmk/events/split_central_status_changed.h>)
#define RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS 1
#include <zmk/events/split_central_status_changed.h>
#else
#define RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS 0
#endif

#include <rawhid_app/uplink.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_STATUS_COUNT 4
#define BATTERY_STATUS_ENTRIES 5
#define BATTERY_REPORT_INTERVAL_MIN 5
#define BATTERY_ENTRY_MAX 4
#define BATTERY_PERIPHERAL_SOURCE_MAX 3
#define BATTERY_CENTRAL_SOURCE 0
#define BATTERY_LEVEL_DISCONNECTED 0xff

static uint8_t central_level = BATTERY_LEVEL_DISCONNECTED;
static uint8_t peripheral_levels[BATTERY_PERIPHERAL_SOURCE_MAX] = {
    BATTERY_LEVEL_DISCONNECTED,
    BATTERY_LEVEL_DISCONNECTED,
    BATTERY_LEVEL_DISCONNECTED,
};
#if RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS
static bool connected[BATTERY_PERIPHERAL_SOURCE_MAX] = {false};
#endif

static void battery_periodic_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_periodic_work, battery_periodic_work_handler);
BUILD_ASSERT(BATTERY_PERIPHERAL_SOURCE_COUNT <= BATTERY_PERIPHERAL_SOURCE_MAX,
             "BATTERY_STATUS can report at most three peripheral sources");

static int send_entries(const uint8_t *sources, const uint8_t *entry_levels, uint8_t count) {
    if (count == 0 || count > BATTERY_ENTRY_MAX) {
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

static void send_central(void) {
    uint8_t source = BATTERY_CENTRAL_SOURCE;
    uint8_t level = central_level;

    send_entries(&source, &level, 1);
}

static void send_peripheral(uint8_t source_index) {
    uint8_t source = source_index + 1;
    uint8_t level = peripheral_levels[source_index];

    send_entries(&source, &level, 1);
}

void rawhid_app_battery_report_send_now(void) {
    uint8_t sources[1 + BATTERY_PERIPHERAL_SOURCE_COUNT];
    uint8_t entry_levels[1 + BATTERY_PERIPHERAL_SOURCE_COUNT];
    uint8_t count = 0;

    sources[count] = BATTERY_CENTRAL_SOURCE;
    entry_levels[count] = central_level;
    count++;

    for (uint8_t i = 0; i < BATTERY_PERIPHERAL_SOURCE_COUNT; i++) {
        sources[count] = i + 1;
        entry_levels[count] = peripheral_levels[i];
        count++;
    }

    send_entries(sources, entry_levels, count);
}

static void schedule_periodic(void) {
    k_work_reschedule(&battery_periodic_work, K_MINUTES(BATTERY_REPORT_INTERVAL_MIN));
}

static void battery_periodic_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    rawhid_app_battery_report_send_now();
    schedule_periodic();
}

static int central_battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t level = MIN(ev->state_of_charge, 100);

    if (central_level != level) {
        central_level = level;
        send_central();
    }

    schedule_periodic();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_central_battery_report, central_battery_listener);
ZMK_SUBSCRIPTION(rawhid_app_central_battery_report, zmk_battery_state_changed);

static int battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev == NULL || ev->source >= BATTERY_PERIPHERAL_SOURCE_COUNT) {
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

    if (peripheral_levels[ev->source] != level) {
        peripheral_levels[ev->source] = level;
        send_peripheral(ev->source);
    }

    schedule_periodic();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_battery_report, battery_listener);
ZMK_SUBSCRIPTION(rawhid_app_battery_report, zmk_peripheral_battery_state_changed);

#if RAWHID_APP_HAS_SPLIT_CENTRAL_STATUS
static int battery_connection_listener(const zmk_event_t *eh) {
    const struct zmk_split_central_status_changed *ev = as_zmk_split_central_status_changed(eh);

    if (ev == NULL || ev->slot >= BATTERY_PERIPHERAL_SOURCE_COUNT) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    connected[ev->slot] = ev->connected;

    if (!ev->connected && peripheral_levels[ev->slot] != BATTERY_LEVEL_DISCONNECTED) {
        peripheral_levels[ev->slot] = BATTERY_LEVEL_DISCONNECTED;
        send_peripheral(ev->slot);
    }

    schedule_periodic();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(rawhid_app_battery_connection_report, battery_connection_listener);
ZMK_SUBSCRIPTION(rawhid_app_battery_connection_report, zmk_split_central_status_changed);
#endif
