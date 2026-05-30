zephyr_include_directories(include)

if(CONFIG_RAWHID_APP)
  zephyr_library()
  zephyr_library_sources(src/dispatch.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_LAYER_CONTROL src/layer_control.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_TIME_SYNC src/time_sync.c)
  zephyr_library_sources_ifdef(CONFIG_RAWHID_APP_AI_USAGE src/ai_usage.c)
endif()
