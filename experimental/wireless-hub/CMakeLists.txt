cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eio_wireless_hub)
target_include_directories(app PRIVATE include src tinycrypt/include)
target_compile_options(app PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/include/hub-compat.h)
target_sources(app PRIVATE src/main.c src/usb.c src/radio.c src/ble-mouse.c src/diagnostics.c
    src/mouse-map.c src/peer-crypto.c src/peer-store.c src/input-merge.c
    src/studio-radio.c src/ph_lite_dongle_at.c src/dongle-led.c
    tinycrypt/source/aes_encrypt.c tinycrypt/source/ccm_mode.c tinycrypt/source/utils.c)
