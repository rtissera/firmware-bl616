# Modifications copyright (c) 2026 Romain Tisserand.
# Originally from nand2mario/firmware-bl616 (Apache-2.0); this file has been
# modified in the pcetang fork.
# SPDX-License-Identifier: Apache-2.0

SDK_DEMO_PATH ?= .
BL_SDK_BASE ?= ../bouffalo_sdk
TANG_BOARD ?= console60k
WIFI_DEBUG ?= 0
# USB CDC debug channel (see usb/cdc_debug.c). Swaps the USB host stack for the device
# stack, so this build has NO gamepad support. Dev only, never shipped.
USB_CDC_DEBUG ?= 0
# 1 = show the Neo Geo (NeoTang, early preview) entry in the core menu. Off in releases.
SHOW_NEOGEO ?= 0
# 1 = debug build: write debug.log to the SD card root. Off in releases.
DEBUG ?= 0

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

cmake_definition+=-DTANG_BOARD=$(TANG_BOARD)
cmake_definition+=-DWIFI_DEBUG=$(WIFI_DEBUG)
cmake_definition+=-DUSB_CDC_DEBUG=$(USB_CDC_DEBUG)
cmake_definition+=-DSHOW_NEOGEO=$(SHOW_NEOGEO)
cmake_definition+=-DDEBUG=$(DEBUG)

include $(BL_SDK_BASE)/project.build

