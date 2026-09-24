/*
 * Copyright (c) 2026 Romain Tisserand
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CDC_DEBUG_H
#define CDC_DEBUG_H
/* USB CDC-ACM debug channel. Only built under USB_CDC_DEBUG=1 -- see cdc_debug.c for
 * why this mode exists and what it gives up (USB host, and therefore gamepads). */
#ifdef __cplusplus
extern "C" {
#endif
void cdc_debug_init(void);   /* bring up the USB DEVICE stack; call instead of usbh_initialize() */
void cdc_debug_poll(void);   /* service one pending command; call from the main loop */
void cdc_debug_puts(const char *s);  /* non-blocking-ish log line; drops if no host listening */
#ifdef __cplusplus
}
#endif
#endif
