/* Native-fixture-only deferred SAVE driver. */
#pragma once
#include <zephyr/sys/util.h>
#if IS_ENABLED(CONFIG_RAWHID_APP_COMBO_SETTINGS_TEST)
void rawhid_app_config_test_run_pending_combo_save(void);
#endif
