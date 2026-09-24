/*
 * Copyright (c) 2026 Romain Tisserand
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB descriptor/setup code adapted from bouffalo_sdk
 * examples/peripherals/usbdev/usbd_cdc_acm/cdc_acm_template.c (Apache-2.0).
 */

/*
 * USB CDC-ACM debug channel for tangcore (2026-09-09).
 *
 * WHY THIS EXISTS: hardware iteration on this board means swapping the SD card for every
 * bitstream and every log read, which is the slowest part of the whole debug loop. The
 * BL616 has exactly ONE USB controller. The bootrom runs it as a DEVICE (that is the
 * 349b:6160 CDC that ISP mode presents, confirmed working at ~3 MB/s), while tangcore's
 * normal firmware runs it as a HOST for gamepads and USB drives. Running the device
 * stack instead gives a live bidirectional link to a PC on the same connector.
 *
 * Built ONLY under `make TANG_BOARD=console60k USB_CDC_DEBUG=1`. The shipped firmware is
 * untouched and keeps USB host / gamepads, which this mode necessarily gives up.
 *
 * STAGE 1 ON PURPOSE: this only enumerates and answers PING with PONG. The USB stack is
 * being swapped underneath the whole firmware, so the first flash proves the STACK works
 * before any command parser, logging, or upload path can muddy the diagnosis. Logging and
 * UPLOAD/CORE commands come in later stages, each its own flash.
 *
 * Adapted from bouffalo_sdk examples/peripherals/usbdev/usbd_cdc_acm/cdc_acm_template.c.
 */
#include "usbd_core.h"
#include "usbd_cdc.h"
#include <string.h>

/*!< endpoint address */
#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x83

#define USBD_VID           0xFFFF
#define USBD_PID           0xFFFF
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

/*!< global descriptor */
static const uint8_t cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02),
    ///////////////////////////////////////
    /// string0 descriptor
    ///////////////////////////////////////
    USB_LANGID_INIT(USBD_LANGID_STRING),
    ///////////////////////////////////////
    /// string1 descriptor
    ///////////////////////////////////////
    0x14,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'C', 0x00,                  /* wcChar0 */
    'h', 0x00,                  /* wcChar1 */
    'e', 0x00,                  /* wcChar2 */
    'r', 0x00,                  /* wcChar3 */
    'r', 0x00,                  /* wcChar4 */
    'y', 0x00,                  /* wcChar5 */
    'U', 0x00,                  /* wcChar6 */
    'S', 0x00,                  /* wcChar7 */
    'B', 0x00,                  /* wcChar8 */
    ///////////////////////////////////////
    /// string2 descriptor
    ///////////////////////////////////////
    0x26,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'C', 0x00,                  /* wcChar0 */
    'h', 0x00,                  /* wcChar1 */
    'e', 0x00,                  /* wcChar2 */
    'r', 0x00,                  /* wcChar3 */
    'r', 0x00,                  /* wcChar4 */
    'y', 0x00,                  /* wcChar5 */
    'U', 0x00,                  /* wcChar6 */
    'S', 0x00,                  /* wcChar7 */
    'B', 0x00,                  /* wcChar8 */
    ' ', 0x00,                  /* wcChar9 */
    'C', 0x00,                  /* wcChar10 */
    'D', 0x00,                  /* wcChar11 */
    'C', 0x00,                  /* wcChar12 */
    ' ', 0x00,                  /* wcChar13 */
    'D', 0x00,                  /* wcChar14 */
    'E', 0x00,                  /* wcChar15 */
    'M', 0x00,                  /* wcChar16 */
    'O', 0x00,                  /* wcChar17 */
    ///////////////////////////////////////
    /// string3 descriptor
    ///////////////////////////////////////
    0x16,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    '2', 0x00,                  /* wcChar0 */
    '0', 0x00,                  /* wcChar1 */
    '2', 0x00,                  /* wcChar2 */
    '2', 0x00,                  /* wcChar3 */
    '1', 0x00,                  /* wcChar4 */
    '2', 0x00,                  /* wcChar5 */
    '3', 0x00,                  /* wcChar6 */
    '4', 0x00,                  /* wcChar7 */
    '5', 0x00,                  /* wcChar8 */
    '6', 0x00,                  /* wcChar9 */
#ifdef CONFIG_USB_HS
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x02,
    0x02,
    0x01,
    0x40,
    0x01,
    0x00,
#endif
    0x00
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_read_buffer[2048];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t cdc_write_buffer[2048];

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

static volatile bool ep_tx_busy_flag = false;
static volatile uint8_t dtr_enable = 0;
/* Set once the host has configured us. Nothing may touch the IN endpoint before this. */
static volatile bool cdc_configured = false;

/* Command bytes accumulate here from the OUT endpoint and are consumed a line at a time.
 * Deliberately tiny: stage 1 understands one command. */
static char cmd_buf[128];
static volatile uint16_t cmd_len = 0;
static volatile bool cmd_ready = false;

void usbd_event_handler(uint8_t event)
{
    switch (event) {
        case USBD_EVENT_CONFIGURED:
            cdc_configured = true;
            usbd_ep_start_read(CDC_OUT_EP, cdc_read_buffer, 2048);
            break;
        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_RESET:
            cdc_configured = false;
            ep_tx_busy_flag = false;
            break;
        default:
            break;
    }
}

void usbd_cdc_acm_bulk_out(uint8_t ep, uint32_t nbytes)
{
    for (uint32_t i = 0; i < nbytes; i++) {
        char c = (char)cdc_read_buffer[i];
        if (c == '\r' || c == '\n') {
            if (cmd_len > 0) {
                cmd_buf[cmd_len] = '\0';
                cmd_ready = true;
            }
        } else if (cmd_len < sizeof(cmd_buf) - 1) {
            cmd_buf[cmd_len++] = c;
        }
        /* Overlong lines are truncated rather than wrapped: a lost command is far easier
         * to notice than a silently mangled one. */
    }
    usbd_ep_start_read(CDC_OUT_EP, cdc_read_buffer, 2048);
}

void usbd_cdc_acm_bulk_in(uint8_t ep, uint32_t nbytes)
{
    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        usbd_ep_start_write(CDC_IN_EP, NULL, 0);   /* ZLP to close a full-MPS transfer */
    } else {
        ep_tx_busy_flag = false;
    }
}

void usbd_cdc_acm_set_dtr(uint8_t intf, bool dtr)
{
    dtr_enable = dtr ? 1 : 0;
}

static struct usbd_endpoint cdc_out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out };
static struct usbd_endpoint cdc_in_ep  = { .ep_addr = CDC_IN_EP,  .ep_cb = usbd_cdc_acm_bulk_in  };

static struct usbd_interface cdc_intf0;
static struct usbd_interface cdc_intf1;

/* Never blocks forever: if the host is not listening (no DTR, or nothing configured) the
 * write is dropped. A debug channel that can hang the firmware is worse than no channel,
 * and this same mistake -- a log sink that blocks -- is why the UART sink was written
 * as a no-op when its device is absent. */
void cdc_debug_puts(const char *s)
{
    uint32_t n = 0;
    if (!cdc_configured || !dtr_enable || s == NULL) return;
    while (s[n] && n < sizeof(cdc_write_buffer) - 2) { cdc_write_buffer[n] = (uint8_t)s[n]; n++; }
    cdc_write_buffer[n++] = '\r';
    cdc_write_buffer[n++] = '\n';

    /* Bounded spin: ~100 ms at worst, then give up rather than wedge the caller. */
    uint32_t guard = 0;
    while (ep_tx_busy_flag && guard++ < 100000) { }
    if (ep_tx_busy_flag) return;

    ep_tx_busy_flag = true;
    usbd_ep_start_write(CDC_IN_EP, cdc_write_buffer, n);
    guard = 0;
    while (ep_tx_busy_flag && guard++ < 100000) { }
}

/* Stage 1: PING -> PONG, and nothing else. Called from the main loop. */
void cdc_debug_poll(void)
{
    if (!cmd_ready) return;

    if (strcmp(cmd_buf, "PING") == 0) {
        cdc_debug_puts("PONG");
    } else {
        cdc_debug_puts("ERR unknown command (stage 1 knows only PING)");
    }

    cmd_len = 0;
    cmd_ready = false;
}

void cdc_debug_init(void)
{
    usbd_desc_register(cdc_descriptor);
    usbd_add_interface(usbd_cdc_acm_init_intf(&cdc_intf0));
    usbd_add_interface(usbd_cdc_acm_init_intf(&cdc_intf1));
    usbd_add_endpoint(&cdc_out_ep);
    usbd_add_endpoint(&cdc_in_ep);
    usbd_initialize();
}
