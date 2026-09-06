#include "wifi_debug.h"

#ifdef TANGCORE_WIFI_DEBUG

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <lwip/tcpip.h>
#include <lwip/sockets.h>

#include "bl_fw_api.h"
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"

#include "bflb_irq.h"
#include "bl616_glb.h"
#include "rfparam_adapter.h"

#include "wifi_creds.h"   // gitignored -- see wifi_creds.h.example

static wifi_conf_t s_wifi_conf = {
    .country_code = "US",
};

static volatile bool s_wifi_connected = false;
static int s_log_sock = -1;
static struct sockaddr_in s_log_addr;

extern "C" void interrupt0_handler(void);

// Called by the WiFi stack itself (bl6_os_adapter/platform_bouffalo_sdk.c calls this
// by name, not via a registered pointer -- real requirement, not a style choice).
extern "C" void wifi_event_handler(uint32_t code) {
    switch (code) {
        case CODE_WIFI_ON_INIT_DONE:
            wifi_mgmr_init(&s_wifi_conf);
            break;
        case CODE_WIFI_ON_MGMR_DONE:
            wifi_mgmr_sta_quickconnect((char *)WIFI_DEBUG_SSID, (char *)WIFI_DEBUG_PASSWORD, 0, 0);
            break;
        case CODE_WIFI_ON_GOT_IP:
            s_wifi_connected = true;
            wifi_log("wifi_debug: got IP, logging live");
            break;
        case CODE_WIFI_ON_DISCONNECT:
            s_wifi_connected = false;
            break;
        default:
            break;
    }
}

static void wifi_debug_init_task(void *param) {
    (void)param;

    GLB_PER_Clock_UnGate(GLB_AHB_CLOCK_IP_WIFI_PHY | GLB_AHB_CLOCK_IP_WIFI_MAC_PHY | GLB_AHB_CLOCK_IP_WIFI_PLATFORM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);

    bflb_irq_attach(WIFI_IRQn, (irq_callback)interrupt0_handler, NULL);
    bflb_irq_enable(WIFI_IRQn);

    // Real: reads WiFi RF calibration from chip eFuse (not a flash partition --
    // checked this before adding WiFi at all, tangcore's flash layout has no
    // partition table). Opt-in debug feature only, so a failure here just means no
    // WiFi logging this boot -- never worth failing the real boot over.
    if (rfparam_init(0, NULL, 0) != 0) {
        vTaskDelete(NULL);
        return;
    }

    tcpip_init(NULL, NULL);

    s_log_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_log_sock >= 0) {
        memset(&s_log_addr, 0, sizeof(s_log_addr));
        s_log_addr.sin_family = AF_INET;
        s_log_addr.sin_port = htons(WIFI_DEBUG_LOG_PORT);
        inet_pton(AF_INET, WIFI_DEBUG_LOG_HOST, &s_log_addr.sin_addr);
    }

    TaskHandle_t fw_task;
    xTaskCreate(wifi_main, (char *)"wifi_fw", 1536, NULL, 16, &fw_task);

    vTaskDelete(NULL);
}

void wifi_debug_start(void) {
    xTaskCreate(wifi_debug_init_task, (char *)"wifi_dbg_init", 4096, NULL, 10, NULL);
}

void wifi_log(const char *msg) {
    if (!s_wifi_connected || s_log_sock < 0)
        return;
    sendto(s_log_sock, msg, strlen(msg), 0, (struct sockaddr *)&s_log_addr, sizeof(s_log_addr));
}

#else  // !TANGCORE_WIFI_DEBUG -- real no-ops, shipped/tested build never touches WiFi

void wifi_debug_start(void) {}
void wifi_log(const char *) {}

#endif
