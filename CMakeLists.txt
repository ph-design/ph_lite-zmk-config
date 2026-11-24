zephyr_library()
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library_sources_ifdef(CONFIG_PH_LITE_DONGLE_AT_CMD src/ph_lite_dongle_at.c)

if(CONFIG_PH_LITE_RECEIVER)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/src/studio)
  zephyr_library_sources(
    boards/arm/ph_lite_receiver/dongle-led.c
    boards/arm/ph_lite_receiver/radio.c
    boards/arm/ph_lite_receiver/reports.c
    boards/arm/ph_lite_receiver/serial.c
  )
endif()

if(CONFIG_BOARD_OT60)
  zephyr_include_directories(include)
  add_subdirectory(modules/drv2605)
  # Observe physical events before ZMK's hold-tap and combo listeners can capture them.
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ot60_haptics.c)
endif()
