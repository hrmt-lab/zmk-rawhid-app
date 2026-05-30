#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <rawhid_app/packet.h>
#include <rawhid_app/time_sync.h>

struct rawhid_app_time_sync_state {
    bool synced;
    int64_t local_base_sec;
    int64_t uptime_base_ms;
    int64_t local_base_days;
    uint8_t weekday;
    enum rawhid_app_time_format format_hint;
    enum rawhid_app_clock_mode clock_mode;
};

static struct rawhid_app_time_sync_state state;
static K_MUTEX_DEFINE(state_lock);

static int64_t floor_div(int64_t num, int64_t den) {
    int64_t q = num / den;
    int64_t r = num % den;

    if (r != 0 && ((r < 0) != (den < 0))) {
        q--;
    }

    return q;
}

static int64_t positive_mod(int64_t num, int64_t den) {
    int64_t mod = num % den;

    if (mod < 0) {
        mod += den;
    }

    return mod;
}

static void civil_from_days(int64_t days, int *year, unsigned int *month, unsigned int *day) {
    days += 719468;
    int64_t era = floor_div(days, 146097);
    unsigned int day_of_era = (unsigned int)(days - era * 146097);
    unsigned int year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int y = (int)year_of_era + (int)era * 400;
    unsigned int day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    unsigned int month_prime = (5 * day_of_year + 2) / 153;

    *day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    *month = month_prime + (month_prime < 10 ? 3 : -9);
    *year = y + (*month <= 2);
}

static int current_local_parts(const struct rawhid_app_time_sync_state *snapshot, int *year,
                               unsigned int *month, unsigned int *day, unsigned int *hour,
                               unsigned int *minute, unsigned int *second, uint8_t *weekday) {
    if (!snapshot->synced) {
        return -ENODATA;
    }

    int64_t elapsed_sec = (k_uptime_get() - snapshot->uptime_base_ms) / 1000;
    int64_t local_sec = snapshot->local_base_sec + elapsed_sec;
    int64_t days = floor_div(local_sec, 86400);
    int64_t seconds_of_day = positive_mod(local_sec, 86400);
    int64_t day_delta = days - snapshot->local_base_days;

    civil_from_days(days, year, month, day);

    *hour = seconds_of_day / 3600;
    *minute = (seconds_of_day % 3600) / 60;
    *second = seconds_of_day % 60;
    *weekday = ((snapshot->weekday - 1 + positive_mod(day_delta, 7)) % 7) + 1;

    return 0;
}

static unsigned int display_hour(unsigned int hour, enum rawhid_app_clock_mode clock_mode) {
    if (clock_mode != RAWHID_APP_CLOCK_12H) {
        return hour;
    }

    hour %= 12;
    return hour == 0 ? 12 : hour;
}

static const char *weekday_name(uint8_t weekday) {
    static const char *const names[] = {
        "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
    };

    return weekday >= 1 && weekday <= 7 ? names[weekday] : names[1];
}

bool rawhid_app_time_sync_format(char *buf, size_t len) {
    struct rawhid_app_time_sync_state snapshot;
    int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    uint8_t weekday;

    if (k_mutex_lock(&state_lock, K_MSEC(5)) < 0) {
        return false;
    }

    snapshot = state;
    k_mutex_unlock(&state_lock);

    if (current_local_parts(&snapshot, &year, &month, &day, &hour, &minute, &second, &weekday) <
        0) {
        return false;
    }

    switch (snapshot.format_hint) {
    case RAWHID_APP_TIME_FORMAT_TIME_HMS:
        snprintf(buf, len, "%02u:%02u:%02u", display_hour(hour, snapshot.clock_mode), minute,
                 second);
        break;
    case RAWHID_APP_TIME_FORMAT_DATE_YMD:
        snprintf(buf, len, "%04d-%02u-%02u", year, month, day);
        break;
    case RAWHID_APP_TIME_FORMAT_DATE_MD:
        snprintf(buf, len, "%02u-%02u", month, day);
        break;
    case RAWHID_APP_TIME_FORMAT_DATETIME_HM:
        snprintf(buf, len, "%04d-%02u-%02u %02u:%02u", year, month, day,
                 display_hour(hour, snapshot.clock_mode), minute);
        break;
    case RAWHID_APP_TIME_FORMAT_WEEKDAY_HM:
        snprintf(buf, len, "%s %02u:%02u", weekday_name(weekday),
                 display_hour(hour, snapshot.clock_mode), minute);
        break;
    case RAWHID_APP_TIME_FORMAT_TIME_HM:
    default:
        snprintf(buf, len, "%02u:%02u", display_hour(hour, snapshot.clock_mode), minute);
        break;
    }

    return true;
}

bool rawhid_app_time_sync_wants_seconds(void) {
    bool wants_seconds;

    if (k_mutex_lock(&state_lock, K_MSEC(5)) < 0) {
        return false;
    }

    wants_seconds = state.synced && state.format_hint == RAWHID_APP_TIME_FORMAT_TIME_HMS;
    k_mutex_unlock(&state_lock);

    return wants_seconds;
}

void rawhid_app_time_sync_handle(const struct rawhid_app_packet *packet) {
    int64_t local_base_sec = ((int64_t)packet->time_sync.unix_time_sec) +
                             ((int64_t)packet->time_sync.tz_offset_min * 60);

    if (k_mutex_lock(&state_lock, K_MSEC(5)) < 0) {
        return;
    }

    state.synced = true;
    state.local_base_sec = local_base_sec;
    state.uptime_base_ms = k_uptime_get();
    state.local_base_days = floor_div(local_base_sec, 86400);
    state.weekday = packet->time_sync.weekday;
    state.format_hint = packet->time_sync.format_hint;
    state.clock_mode = packet->time_sync.clock_mode;

    k_mutex_unlock(&state_lock);
}
