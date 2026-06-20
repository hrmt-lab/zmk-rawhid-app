/*
 * Override the ZMK Studio core/get_device_info handler so that the response's
 * `serial_number` carries the same stable device UID that RawHID Host Link
 * advertises as `device_uid_hash` (see src/identity.c, src/dispatch.c).
 *
 * Upstream zmk (app/src/studio/core_subsystem.c) fills `serial_number` with the
 * raw `hwinfo_get_device_id()` bytes, which (a) differs in representation from
 * `device_uid_hash` and (b) is empty on devices without hwinfo. Here we instead
 * emit the 16-char lowercase hex of `rawhid_app_identity_get_uid_hash()`, so the
 * host can match a Studio device to its Host Link identity by string compare.
 *
 * Override mechanism: ZMK dispatches to the *first* handler in the
 * zmk_rpc_subsystem_handler iterable section whose request_choice matches
 * (zmk/app/src/studio/rpc.c). That section is laid out by the linker with
 * SORT_BY_NAME, i.e. sorted by the STRUCT_SECTION_ITERABLE variable name. The
 * variable name below ("core_subsystem_handler_00_get_device_info") is
 * load-bearing: it must sort *before* upstream's
 * "core_subsystem_handler_get_device_info" (so ours wins) while keeping the
 * "core_subsystem_handler_" prefix (so it stays inside the contiguous core
 * block that zmk_rpc_init() groups). Do not rename it casually.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pb_encode.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

#include <rawhid_app/identity.h>

static bool encode_device_info_name(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, CONFIG_ZMK_KEYBOARD_NAME, strlen(CONFIG_ZMK_KEYBOARD_NAME));
}

static bool encode_device_info_serial_number(pb_ostream_t *stream, const pb_field_t *field,
                                             void *const *arg) {
    static const char hexd[] = "0123456789abcdef";
    uint64_t uid = rawhid_app_identity_get_uid_hash();

    if (uid == 0) {
        /* 0 means identity unavailable (neither hwinfo nor settings resolved a
         * seed). We still emit a non-empty serial_number, but multiple such
         * devices would all collide on "0000000000000000", so warn loudly. */
        LOG_WRN("rawhid studio: uid_hash==0 (identity unavailable); serial_number not unique");
    }

    /* Most-significant nibble first (matches printf %016llx). Fixed 16 chars. */
    char hex[16];
    for (int i = 0; i < 16; i++) {
        hex[15 - i] = hexd[uid & 0xf];
        uid >>= 4;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, (const uint8_t *)hex, sizeof(hex));
}

static zmk_studio_Response rawhid_get_device_info(const zmk_studio_Request *req) {
    zmk_core_GetDeviceInfoResponse resp = zmk_core_GetDeviceInfoResponse_init_zero;

    resp.name.funcs.encode = encode_device_info_name;
    resp.serial_number.funcs.encode = encode_device_info_serial_number;

    return ZMK_RPC_RESPONSE(core, get_device_info, resp);
}

/* See file header: name must sort before "core_subsystem_handler_get_device_info". */
STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, core_subsystem_handler_00_get_device_info) = {
    .func = rawhid_get_device_info,
    .subsystem_choice = zmk_studio_Request_core_tag,
    .request_choice = zmk_core_Request_get_device_info_tag,
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};
