/* Test-only internal interface. Never installed under include/. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
int rawhid_app_combo_settings_test_load_image(const uint8_t *image, size_t length);
int rawhid_app_combo_settings_test_load_missing(void);
int rawhid_app_combo_settings_test_load_read_error(void);
#endif
