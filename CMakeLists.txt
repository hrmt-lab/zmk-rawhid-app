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

if(CONFIG_RAWHID_APP_CONFIG_RPC)
  target_sources(app PRIVATE src/encoder_runtime.c)
  target_sources(app PRIVATE src/behavior_identity.c)
endif()

# This is deliberately compiled for every build that discovers this module:
# a disabled /combos node without the Keylink runtime would silently remove
# all Combo behavior.
target_sources(app PRIVATE src/combo_runtime_contract.c)

if(CONFIG_RAWHID_APP_COMBO_RUNTIME)
  target_sources(app PRIVATE src/combo_runtime.c)

  add_custom_target(rawhid_app_combo_listener_order_check ALL
    COMMAND ${CMAKE_COMMAND}
      -DMAP_FILE=${CMAKE_BINARY_DIR}/zephyr/zmk.map
      -DDT_HEADER_FILE=${CMAKE_BINARY_DIR}/zephyr/include/generated/zephyr/devicetree_generated.h
      -P ${CMAKE_CURRENT_LIST_DIR}/cmake/verify_combo_listener_order.cmake
    DEPENDS ${CMAKE_BINARY_DIR}/zephyr/zmk.elf
    VERBATIM
  )
endif()

if(CONFIG_RAWHID_APP)
  zephyr_library()
  zephyr_library_sources(src/dispatch.c)
  zephyr_library_sources(src/ai_client_state_packet.c)
  zephyr_library_sources(src/identity.c)
  zephyr_library_sources(src/uplink.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_LAYER_CONTROL src/layer_control.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_TIME_SYNC src/time_sync.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_USAGE src/ai_usage.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_CLIENT_STATE src/ai_client_state.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_CLIENT_STATE src/ai_client_state_model.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_CLIENT_STATE src/events/ai_client_state_changed.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_BATTERY_REPORT src/battery_report.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_KEY_STATS src/key_stats.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_KEY_PRESS src/key_press.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_LAYER_STATE_REPORT src/layer_state_report.c)
endif()
