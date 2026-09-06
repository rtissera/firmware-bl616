SDK_DEMO_PATH ?= .
BL_SDK_BASE ?= ../bouffalo_sdk
TANG_BOARD ?= console60k
WIFI_DEBUG ?= 0

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

cmake_definition+=-DTANG_BOARD=$(TANG_BOARD)
cmake_definition+=-DWIFI_DEBUG=$(WIFI_DEBUG)

include $(BL_SDK_BASE)/project.build

