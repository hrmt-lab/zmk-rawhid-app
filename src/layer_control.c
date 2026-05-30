#include <stdbool.h>

#include <zmk/keymap.h>

#include <zephyr/logging/log.h>

#include <rawhid_app/packet.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static zmk_keymap_layer_id_t managed_layer = ZMK_KEYMAP_LAYER_ID_INVAL;
static bool managed_layer_activated_by_host;

static void clear_host_layer(void) {
    if (managed_layer != ZMK_KEYMAP_LAYER_ID_INVAL && managed_layer_activated_by_host) {
        int err = zmk_keymap_layer_deactivate(managed_layer, false);
        if (err < 0) {
            LOG_WRN("Failed to deactivate host-managed layer %u: %d", managed_layer, err);
        }
    }

    managed_layer = ZMK_KEYMAP_LAYER_ID_INVAL;
    managed_layer_activated_by_host = false;
}

static void set_host_layer(zmk_keymap_layer_id_t layer) {
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_WRN("Ignoring host layer %u, only %u layers exist", layer, ZMK_KEYMAP_LAYERS_LEN);
        return;
    }

    bool same_host_activated_layer = managed_layer == layer && managed_layer_activated_by_host;

    if (managed_layer != ZMK_KEYMAP_LAYER_ID_INVAL && managed_layer != layer &&
        managed_layer_activated_by_host) {
        int err = zmk_keymap_layer_deactivate(managed_layer, false);
        if (err < 0) {
            LOG_WRN("Failed to deactivate previous host-managed layer %u: %d", managed_layer, err);
        }
    }

    managed_layer = layer;

    if (zmk_keymap_layer_active(layer)) {
        managed_layer_activated_by_host = same_host_activated_layer;
        return;
    }

    int err = zmk_keymap_layer_activate(layer, false);
    if (err < 0) {
        managed_layer = ZMK_KEYMAP_LAYER_ID_INVAL;
        managed_layer_activated_by_host = false;
        LOG_WRN("Failed to activate host layer %u: %d", layer, err);
        return;
    }

    managed_layer_activated_by_host = true;
}

void rawhid_app_layer_control_handle(const struct rawhid_app_packet *packet) {
    if (packet->type != RAWHID_APP_PACKET_APP_LAYER) {
        return;
    }

    switch (packet->app_layer.action) {
    case RAWHID_APP_APP_LAYER_SET:
        set_host_layer(packet->app_layer.layer);
        break;
    case RAWHID_APP_APP_LAYER_CLEAR:
        clear_host_layer();
        break;
    default:
        break;
    }
}
