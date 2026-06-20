zephyr_include_directories(include)

if(CONFIG_DT_HAS_ZMK_BEHAVIOR_HOST_ACTION_ENABLED)
  target_sources(app PRIVATE src/behaviors/behavior_host_action.c)
endif()

# Studio get_device_info override. Compiled into `app` (not the zephyr_library
# below) so it inherits the app's generated-proto include path and build
# ordering for <proto/zmk/*.pb.h>.
if(CONFIG_RAWHID_APP AND CONFIG_ZMK_STUDIO_RPC)
  target_sources(app PRIVATE src/studio_identity.c)
endif()

if(CONFIG_RAWHID_APP)
  zephyr_library()
  zephyr_library_sources(src/dispatch.c)
  zephyr_library_sources(src/identity.c)
  zephyr_library_sources(src/uplink.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_LAYER_CONTROL src/layer_control.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_TIME_SYNC src/time_sync.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_USAGE src/ai_usage.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_BATTERY_REPORT src/battery_report.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_KEY_STATS src/key_stats.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_KEY_PRESS src/key_press.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_LAYER_STATE_REPORT src/layer_state_report.c)
endif()
