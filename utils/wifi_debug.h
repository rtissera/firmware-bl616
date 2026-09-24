/*
 * Copyright (c) 2026 Romain Tisserand
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Dev-only WiFi UDP debug logging -- only built when compiled with WIFI_DEBUG=1
// (see CMakeLists.txt). Both functions are safe to call unconditionally either way:
// they're real no-ops in a normal build. Real reason this exists: reading debug.log
// off the SD card means physically swapping it between the board and this PC on every
// iteration; this sends the same kind of log line live over UDP to a dev PC on the
// same WiFi network instead.
//
// wifi_log() does a real (non-blocking, best-effort) UDP send -- it is NOT safe to
// call from inside a taskENTER_CRITICAL()/taskEXIT_CRITICAL() section (fpga_program()'s
// JTAG streaming loop is the one that matters here): WiFi's own stack is
// interrupt-driven and a critical section stalls it, so file_log() to the SD card
// stays the real logging path for that specific window. wifi_log() is for the code
// that runs after a core is already loaded and interrupts are back on.

void wifi_debug_start(void);          // call once from main(), after board/task init
void wifi_log(const char *msg);
