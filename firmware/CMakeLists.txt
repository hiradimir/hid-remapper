cmake_minimum_required(VERSION 3.13)

add_subdirectory(usb_midi_host)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)
add_compile_definitions(PERSISTED_CONFIG_SIZE=4096)
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")
set(PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/src/boards")

include(pico_sdk_import.cmake)

project(remapper)

# 2097152 (2MB) - 3 * 4096 (12KB) = 2084864 (TLV領域を hid-remapper config の手前に配置)
add_compile_definitions(PICO_FLASH_BANK_STORAGE_OFFSET=2084864)

pico_sdk_init()

check_and_add_pico_pio_usb_support()

add_compile_options(-Wall)

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)

if((PICO_BOARD STREQUAL "pico") OR (PICO_BOARD STREQUAL "pico2") OR (PICO_BOARD STREQUAL "pico_w") OR (PICO_BOARD STREQUAL "pico2_w"))
add_compile_definitions(GPIO_VALID_PINS_BASE=0b00011100011111111111111111111111)
endif()


option(ENABLE_BLE "Enable Bluetooth Low Energy support (requires pico_w)")
option(ENABLE_OLED "Enable SSD1306 OLED display" OFF)
option(ENABLE_REMOTE_LOG "Enable Wi-Fi (UDP) remote logging (requires pico_w + src/remote_log_net.h)" OFF)
# BL-45 切り分け専用: 1 にすると ble_lock/ble_unlock を no-op にする (常用は OFF)
option(BLE_LOCK_DISABLE "Disable ble_lock (BL-45 bisect only)" OFF)

if(ENABLE_BLE AND NOT (PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w" OR PICO_PLATFORM STREQUAL "rp2350-arm-s"))
    message(FATAL_ERROR "ENABLE_BLE requires PICO_BOARD=pico_w/pico2_w or an RP2350-based CYW43 board")
endif()


add_executable(remapper
    src/main.cc
    src/remapper.cc
    src/remapper_single.cc
    src/crc.cc
    src/descriptor_parser.cc
    src/tinyusb_stuff.cc
    src/our_descriptor.cc
    src/globals.cc
    src/config.cc
    src/quirks.cc
    src/interval_override.cc
    src/out_report.cc
    src/tick.cc
    src/activity_led.cc
    src/ps_auth.cc
    src/app_driver.cc
    src/xbox.cc
)

if((PICO_BOARD STREQUAL "pico") OR (PICO_BOARD STREQUAL "pico2") OR (PICO_BOARD STREQUAL "pico_w") OR (PICO_BOARD STREQUAL "pico2_w"))
target_compile_definitions(remapper PUBLIC PICO_DEFAULT_UART_TX_PIN=16)
target_compile_definitions(remapper PUBLIC PICO_DEFAULT_UART_RX_PIN=17)
target_compile_definitions(remapper PUBLIC PICO_DEFAULT_PIO_USB_DP_PIN=0)
endif()

if(DEFINED PICO_DUAL_PIO_USB_DP_PIN AND NOT "${PICO_DUAL_PIO_USB_DP_PIN}" STREQUAL "")
    target_compile_definitions(remapper PUBLIC ENABLE_DUAL_HOST=1)
    target_compile_definitions(remapper PUBLIC PICO_DUAL_PIO_USB_DP_PIN=${PICO_DUAL_PIO_USB_DP_PIN})
endif()

if(ENABLE_BLE)
    target_sources(remapper PRIVATE src/ble_hid.cc)
    target_compile_definitions(remapper PUBLIC ENABLE_BLE=1)
    if(BLE_LOCK_DISABLE)
        target_compile_definitions(remapper PUBLIC BLE_LOCK_DISABLE=1)
    endif()

    if(ENABLE_OLED)
        target_sources(remapper PRIVATE src/oled.cc)
        target_compile_definitions(remapper PUBLIC ENABLE_OLED=1)
        target_link_libraries(remapper hardware_i2c)
    endif()

    # GATT コンパイルルールの定義 (ble_mouse.gatt -> ble_mouse.h)
    set(GATT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/ble_mouse.h)
    add_custom_command(
        OUTPUT ${GATT_HEADER}
        COMMAND python3 ${PICO_SDK_PATH}/lib/btstack/tool/compile_gatt.py
                ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_mouse.gatt
                ${GATT_HEADER}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_mouse.gatt
        COMMENT "Compiling ble_mouse.gatt to ble_mouse.h"
        VERBATIM
    )
    target_sources(remapper PRIVATE ${GATT_HEADER})
    target_include_directories(remapper PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

    # BTstack / CYW43 ライブラリのリンク
    target_link_libraries(remapper
        pico_btstack_ble
        pico_btstack_cyw43
        hardware_flash
        pico_btstack_flash_bank
        pico_multicore
    )

    if(ENABLE_REMOTE_LOG)
        # BL-39: Wi-Fi (lwIP) を同居させる。cyw43 arch を none → lwip_threadsafe_background に差し替える
        # (CHW43 の non-BLE 操作・async_context 機構は共通のため BLE 側の体制は変化しない)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/remote_log_net.h)
            message(FATAL_ERROR "ENABLE_REMOTE_LOG requires src/remote_log_net.h (copy from src/remote_log_net.h.example)")
        endif()
        target_sources(remapper PRIVATE src/remote_log.cc)
        target_compile_definitions(remapper PUBLIC ENABLE_REMOTE_LOG=1)
        target_link_libraries(remapper pico_cyw43_arch_lwip_threadsafe_background)
    else()
        target_compile_definitions(remapper PUBLIC ENABLE_REMOTE_LOG=0)
        target_link_libraries(remapper pico_cyw43_arch_none)
    endif()
endif()

# ビルド識別子: 焼いた UF2 がどのビルドかをログで確認できるようにする。
# 値は毎ビルド走る custom target でヘッダに書き出す (configure 時に固定すると
# cmake を再実行するまで古い日時/SHA のままになる)
set(REMAPPER_BUILD_ID_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/build_id.h)
add_custom_target(build_id_gen ALL
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUT_HEADER=${REMAPPER_BUILD_ID_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
    BYPRODUCTS ${REMAPPER_BUILD_ID_HEADER}
    COMMENT "Generating build id header")
add_dependencies(remapper build_id_gen)

target_include_directories(remapper PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    src
    src/tusb_config_both
    ${PICO_PIO_USB_PATH}
)

target_link_libraries(remapper
    pico_stdlib
    pico_unique_id
    hardware_pio
    hardware_dma
    hardware_flash
    $<$<STREQUAL:${PICO_BOARD},remapper_v8>:hardware_adc>
    tinyusb_device
    tinyusb_host
    tinyusb_board
    tinyusb_pico_pio_usb
    usb_midi_host
)

pico_enable_stdio_uart(remapper 1)
pico_enable_stdio_usb(remapper 0)

if(ENABLE_BLE)
    pico_set_binary_type(remapper default)
else()
    pico_set_binary_type(remapper copy_to_ram)
    if(PICO_PLATFORM STREQUAL "rp2040")
        pico_set_linker_script(remapper ${CMAKE_CURRENT_LIST_DIR}/remapper_single.ld)
    elseif(PICO_PLATFORM STREQUAL "rp2350-arm-s")
        pico_set_linker_script(remapper ${CMAKE_CURRENT_LIST_DIR}/remapper_single_rp2350.ld)
    else()
        message(FATAL_ERROR "Unknown PICO_PLATFORM.")
    endif()
endif()

pico_add_extra_outputs(remapper)


add_executable(remapper_dual_a
    src/main.cc
    src/remapper.cc
    src/remapper_dual_a.cc
    src/crc.cc
    src/descriptor_parser.cc
    src/tinyusb_stuff.cc
    src/our_descriptor.cc
    src/globals.cc
    src/config.cc
    src/quirks.cc
    src/interval_override.cc
    src/serial.cc
    src/tick.cc
    src/activity_led.cc
    src/pico_debug/swd.c
    src/pico_debug/flash.c
    src/pico_debug/adi.c
    dual_b_binary.c
    src/ps_auth.cc
    $<$<STREQUAL:${PICO_BOARD},remapper_v7>:src/i2c.cc>
    $<$<STREQUAL:${PICO_BOARD},remapper_v7>:src/mcp4651.cc>
)
target_include_directories(remapper_dual_a PRIVATE
    src
    src/tusb_config_device
    src/pico_debug
)
target_link_libraries(remapper_dual_a
    pico_stdlib
    hardware_flash
    $<$<STREQUAL:${PICO_BOARD},remapper_v7>:hardware_i2c>
    hardware_pio
    tinyusb_device
    tinyusb_board
    pico_unique_id
)
pico_generate_pio_header(remapper_dual_a ${CMAKE_CURRENT_LIST_DIR}/src/pico_debug/swd.pio)
pico_add_extra_outputs(remapper_dual_a)

add_executable(remapper_dual_b
    src/remapper_dual_b.cc
    src/crc.cc
    src/interval_override.cc
    src/serial.cc
    src/out_report.cc
    src/activity_led.cc
    src/app_driver.cc
    src/xbox.cc
)
target_include_directories(remapper_dual_b PRIVATE
    src
    src/tusb_config_host
)
target_link_libraries(remapper_dual_b
    pico_stdlib
    tinyusb_host
    tinyusb_board
    usb_midi_host
)
pico_add_extra_outputs(remapper_dual_b)
pico_add_hex_output(remapper_dual_b)

add_custom_command(
  OUTPUT dual_b_binary.c dual_b_binary.h
  DEPENDS remapper_dual_b
  COMMAND srec_cat remapper_dual_b.hex -Intel -Output dual_b_binary.c -C-Array dual_b_binary -INClude
  VERBATIM
)

add_executable(remapper_serial
    src/main.cc
    src/remapper.cc
    src/remapper_serial.cc
    src/crc.cc
    src/descriptor_parser.cc
    src/tinyusb_stuff.cc
    src/our_descriptor.cc
    src/globals.cc
    src/config.cc
    src/quirks.cc
    src/interval_override.cc
    src/tick.cc
    src/activity_led.cc
    src/ps_auth.cc
)
target_include_directories(remapper_serial PRIVATE
    src
    src/tusb_config_device
)
target_link_libraries(remapper_serial
    pico_stdlib
    pico_unique_id
    hardware_flash
    tinyusb_device
    tinyusb_board
)
pico_add_extra_outputs(remapper_serial)

add_executable(flash_b_side
    src/flash_b_side.cc
    dual_b_binary.c
    src/pico_debug/swd.c
    src/pico_debug/flash.c
    src/pico_debug/adi.c
)
target_include_directories(flash_b_side PRIVATE
    src
    src/pico_debug
)
target_link_libraries(flash_b_side
    pico_stdlib
    hardware_pio
)
pico_generate_pio_header(flash_b_side ${CMAKE_CURRENT_LIST_DIR}/src/pico_debug/swd.pio)
pico_add_extra_outputs(flash_b_side)
pico_set_binary_type(flash_b_side no_flash)

add_custom_command(
    OUTPUT remapper_dual_combined.uf2
    DEPENDS remapper_dual_a flash_b_side
    COMMAND ${CMAKE_CURRENT_LIST_DIR}/combine_uf2.py remapper_dual_a.uf2 flash_b_side.uf2 remapper_dual_combined.uf2
    VERBATIM
)

add_custom_target(remapper_dual_combined ALL
    DEPENDS remapper_dual_combined.uf2
)
