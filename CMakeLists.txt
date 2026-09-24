cmake_minimum_required(VERSION 3.15)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CONFIG_FREERTOS 1)
set(CONFIG_CHERRYUSB 1)
# BL616 has ONE USB controller, so host and device are mutually exclusive. The shipped
# build is HOST (gamepads, USB drives); USB_CDC_DEBUG swaps it for DEVICE to get a live
# CDC link to a PC. See usb/cdc_debug.c.
if(USB_CDC_DEBUG)
    set(CONFIG_CHERRYUSB_DEVICE 1)
    set(CONFIG_CHERRYUSB_DEVICE_CDC 1)
    add_definitions(-DTANGCORE_USB_CDC_DEBUG=1)
else()
    set(CONFIG_CHERRYUSB_HOST 1)
endif()
# Neo Geo (NeoTang) menu entry: hidden unless `make ... SHOW_NEOGEO=1` (early preview).
if(SHOW_NEOGEO)
    add_definitions(-DSHOW_NEOGEO=1)
endif()
set(CONFIG_BSP_SDH_SDCARD 1)
set(CONFIG_FATFS 1)
# FatFs-over-USB-host only makes sense with the host stack.
if(NOT USB_CDC_DEBUG)
    set(CONFIG_FATFS_USBH 1)
endif()
set(CONFIG_FATFS_SDH_SDCARD 1)

# Dev-only WiFi UDP debug logging (real hardware iteration without SD-card swaps).
# OFF by default: NOT part of the real/shipped build, only for `make TANG_BOARD=... WIFI_DEBUG=1`.
# See utils/wifi_debug.cpp -- these are the exact components examples/wifi/sta/wifi_udp
# needs (LWIP+WIFI6+RF for the stack, MBEDTLS for the real WPA2 handshake, TLSF for
# lwIP's own heap), real, not optional -- there's no smaller working set that connects
# to a WPA2 AP. See pcetang project memory for the RAM-budget check before enabling
# this on a board any tighter than Console 60K's.
if(WIFI_DEBUG)
    set(TANGCORE_WIFI_DEBUG 1)
    add_definitions(-DTANGCORE_WIFI_DEBUG=1)
    # CONFIG_POSIX deliberately omitted: our code calls no POSIX APIs (the reference
    # wifi_udp example only needs it for its shell/CLI), and it drags in
    # FreeRTOS_POSIX headers that (a) collide with our own utils.h by filename and
    # (b) trip this toolchain's libstdc++ gthr-default.h in a C++17 build -- every
    # SDK WiFi example is C-only, so nobody hit this combination before. Try
    # without it first.
    set(CONFIG_TLSF 1)
    set(CONFIG_LWIP 1)
    set(CONFIG_WIFI6 1)
    set(CONFIG_RF 1)
    # libwifi6.a is a real prebuilt monolithic static lib -- it references AP-mode/
    # shell/DHCP-server glue (stats_display, dhcpd_start, bl_rand) unconditionally,
    # even for STA-only use. Real link errors without these, found empirically.
    set(CONFIG_BFLOG 1)
    set(CONFIG_SHELL 1)
    set(CONFIG_DHCPD 1)
    set(CONFIG_PING 1)
    set(CONFIG_MBEDTLS 1)
    set(CONFIG_MBEDTLS_AES_USE_HW 1)
    set(CONFIG_MBEDTLS_BIGNUM_USE_HW 1)
    set(CONFIG_MBEDTLS_ECC_USE_HW 1)
    set(CONFIG_MBEDTLS_SHA1_USE_HW 1)
    set(CONFIG_MBEDTLS_SHA256_USE_HW 1)
    set(CONFIG_MBEDTLS_SHA512_USE_HW 1)
    set(CONFIG_VIF_MAX 1)
    set(CONFIG_STA_MAX 1)
endif()

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138k")
    add_definitions(-DTANG_MEGA138K)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
endif()

# libchdr: LOWRAM_TARGET on (real ~365KB free SRAM budget on this board, see
# project memory - whole-hunk decode does not fit, streaming-decode design does)
add_definitions(
    -DLOWRAM_TARGET=1
    -DWANT_RAW_DATA_SECTOR=1
    -DWANT_SUBCODE=1
    -DVERIFY_BLOCK_CRC=1
    -DMINIZ_NO_ARCHIVE_APIS
    -DMINIZ_NO_DEFLATE_APIS
    -DMINIZ_NO_STDIO
    -DMINIZ_NO_TIME
    # FLAC backend: micro-flac instead of dr_flac, per libchdr's own
    # contrib/tangcore-bl616 reference integration. Byte-identical output; faster and
    # lower peak heap on every board it has been measured on (ESP32-S3 1.198x on a
    # CD-FLAC hunk, RP2350 1.032x overall). NOT measured on BL616 -- taken on the
    # strength of the other two, and reversible by dropping these two defines and
    # putting libchdr_flac.c back in the glob below.
    -DCHDR_FLAC_BACKEND_MICROFLAC
    -DMICRO_FLAC_DISABLE_OGG
)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Add include directories
sdk_add_include_directories(
    . usb fpga ui core utils chd
    thirdparty/libchdr/include
    thirdparty/libchdr/src
    thirdparty/micro-flac/include
)

if(WIFI_DEBUG)
    # lwIP needs a project-supplied lwipopts.h on the include path; not auto-added by
    # CONFIG_LWIP here (tangcore doesn't use the SDK's own project.build include(proj.conf)
    # path the WiFi examples use). Point straight at the SDK's own default lwip-port
    # config instead of guessing at the examples' lwipopts_user.h indirection.
    sdk_add_include_directories($ENV{BL_SDK_BASE}/components/net/lwip/lwip/lwip-port/config)
endif()

# Add source files
file(GLOB_RECURSE MY_SOURCES
    "usb/*.cpp"
    "fpga/*.cpp"
    "ui/*.cpp"
    "core/*.cpp"
    "utils/*.cpp"
)

if(USB_CDC_DEBUG)
    # usb_gamepad.cpp is pure USB-host code and will not link without the host stack.
    # No stub is needed for it: its only caller in main.cpp is inside the #else branch,
    # and hid1_state/hid2_state live in utils/utils.cpp, not in the driver.
    list(REMOVE_ITEM MY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/usb/usb_gamepad.cpp")
    list(APPEND MY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/usb/cdc_debug.c")
endif()

file(GLOB LIBCHDR_SOURCES
    "chd/*.c"
    "thirdparty/libchdr/src/*.c"
    "thirdparty/libchdr/deps/lzma-26.02/src/LzmaDec.c"
    "thirdparty/libchdr/deps/miniz-3.1.2/miniz.c"
    "thirdparty/libchdr/deps/zstd-1.5.7/zstddeclib.c"
)

# Exactly ONE FLAC backend is compiled. libchdr_flac.c is the dr_flac one, so it has to
# leave the glob when micro-flac comes in, or both define the same symbols.
list(REMOVE_ITEM LIBCHDR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libchdr/src/libchdr_flac.c")

set(LIBCHDR_MICROFLAC_SOURCES
    "thirdparty/libchdr/src/libchdr_flac_microflac.cpp"
    "thirdparty/micro-flac/src/flac_decoder.cpp"
    "thirdparty/micro-flac/src/decorrelation.cpp"
    "thirdparty/micro-flac/src/frame_header.cpp"
    "thirdparty/micro-flac/src/pcm_packing.cpp"
    "thirdparty/micro-flac/src/crc.cpp"
    "thirdparty/micro-flac/src/lpc.cpp"
)

target_sources(app PRIVATE ${MY_SOURCES} ${LIBCHDR_SOURCES} ${LIBCHDR_MICROFLAC_SOURCES})
sdk_set_main_file(main.cpp)

project(tangcore CXX)

