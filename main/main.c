/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/*  WiFi softAP & station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"

#include "log_capture.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"

#include "tailscale_config.h"
#include "tailscale_mtu.h"
#include "telemetry.h"
#include "lwip_route_hook.h"
#include "web_ui.h"
#include "acl.h"
#include "netif_hooks.h"
#include "syslog_client.h"

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_ESP_WIFI_STA_SSID "mywifissid"
*/

/* STA Configuration */
#define EXAMPLE_ESP_WIFI_STA_SSID           CONFIG_ESP_WIFI_REMOTE_AP_SSID
#define EXAMPLE_ESP_WIFI_STA_PASSWD         CONFIG_ESP_WIFI_REMOTE_AP_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY           CONFIG_ESP_MAXIMUM_STA_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD   WIFI_AUTH_WAPI_PSK
#endif

/* AP Configuration */
#define EXAMPLE_ESP_WIFI_AP_SSID            CONFIG_ESP_WIFI_AP_SSID
#define EXAMPLE_ESP_WIFI_AP_PASSWD          CONFIG_ESP_WIFI_AP_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL            CONFIG_ESP_WIFI_AP_CHANNEL
#define EXAMPLE_MAX_STA_CONN                CONFIG_ESP_MAX_STA_CONN_AP


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/*DHCP server option*/
#define DHCPS_OFFER_DNS             0x02

static const char *TAG_AP = "WiFi SoftAP";
static const char *TAG_STA = "WiFi Sta";

static int s_retry_num = 0;

/* FreeRTOS event group to signal when we are connected/disconnected */
static EventGroupHandle_t s_wifi_event_group;

/* Cross-module state. ap_connect tracks whether the upstream STA link
 * is up (telemetry waits for it before its first send); connect_count
 * is the live count of AP clients (rendered on the Status page and
 * reported in telemetry). */
int ap_connect   = 0;
int connect_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
        connect_count++;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                 MAC2STR(event->mac), event->aid, event->reason);
        if (connect_count > 0) connect_count--;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG_STA, "Station started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ap_connect = 0;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        ap_connect = 1;
        /* Auto-spawn the Tailscale (microlink) connect task once STA has
         * an IP — same trigger point as the old repo. The task is
         * responsible for waiting on SNTP, dialing the control plane and
         * staying online; we just kick it off here. */
        if (tailscale_enabled) {
            xTaskCreate(tailscale_connect_task, "ts_connect", 4096, NULL, 5, NULL);
        }
        /* Tell the syslog forwarder to (re-)resolve its server now that
         * the network is up — no-op if syslog isn't enabled. */
        syslog_notify_connected();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD, EXAMPLE_ESP_WIFI_CHANNEL);

    return esp_netif_ap;
}

/* Initialize wifi station */
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_STA_SSID,
            .password = EXAMPLE_ESP_WIFI_STA_PASSWD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = EXAMPLE_ESP_MAXIMUM_RETRY,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
            * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

    ESP_LOGI(TAG_STA, "wifi_init_sta finished.");

    return esp_netif_sta;
}

void softap_set_dns_addr(esp_netif_t *esp_netif_ap,esp_netif_t *esp_netif_sta)
{
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(esp_netif_sta,ESP_NETIF_DNS_MAIN,&dns);
    uint8_t dhcps_offer_option = DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(esp_netif_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(esp_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_option, sizeof(dhcps_offer_option)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(esp_netif_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(esp_netif_ap));
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Install the ESP_LOG ring buffer + RTC-NOINIT pre-crash buffer as
     * early as possible so the boot-time output is recoverable from
     * the /log page (live ring) and from /diag after a PANIC/WDT
     * (the pre-crash slice in slow RTC RAM survives reboot). */
    log_capture_init(0);

    /* Tailscale (microlink) settings — separate NVS keys (ts_*); loader
     * lives in tailscale_manager.c. Microlink lifecycle wires in later
     * once WiFi STA has an IP. */
    tailscale_init();

    /* MTU / MSS clamp / PMTU manager. Owns the wg netif MTU plus the AP
     * hook clamp values. Loads NVS now; the 30 s poll timer takes over
     * once microlink + the wg netif exist. */
    tailscale_mtu_init();

    /* Anonymous telemetry — privacy-respecting flash/boot/heartbeat
     * reporter. Spawns a low-priority task that waits for ap_connect
     * before its first send. */
    telemetry_init();

    /* Exit-node default-route supervisor. Background task flips
     * netif_default between STA and the WireGuard netif depending on
     * tailscale_exit_node_ip. Self-paces until netifs exist. */
    lwip_route_hook_init();

    /* ACL firewall — initialise the in-memory rule tables, then load any
     * persisted rules from NVS. The actual packet-filter hook lands in
     * the netif_hooks slice; this just makes the rules queryable. */
    load_acl_rules();

    /* Remote syslog UDP forwarder. Loads NVS config and installs the
     * vprintf hook if previously enabled. Waits for STA IP via
     * syslog_notify_connected (wired below in the IP event handler). */
    syslog_init();

    /* Initialize event group */
    s_wifi_event_group = xEventGroupCreate();

    /* Register Event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_event_handler,
                    NULL,
                    NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler,
                    NULL,
                    NULL));

    /*Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Initialize AP */
    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    esp_netif_t *esp_netif_ap = wifi_init_softap();

    /* Initialize STA */
    ESP_LOGI(TAG_STA, "ESP_WIFI_MODE_STA");
    esp_netif_t *esp_netif_sta = wifi_init_sta();

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start() );

    /*
     * Wait until either the connection is established (WIFI_CONNECTED_BIT) or
     * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT).
     * The bits are set by event_handler() (see above)
     */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned,
     * hence we can test which event actually happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_STA, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
        softap_set_dns_addr(esp_netif_ap,esp_netif_sta);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_STA, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_STA_SSID, EXAMPLE_ESP_WIFI_STA_PASSWD);
    } else {
        ESP_LOGE(TAG_STA, "UNEXPECTED EVENT");
        return;
    }

    /* Set sta as the default interface */
    esp_netif_set_default_netif(esp_netif_sta);

    /* Enable napt on the AP netif */
    if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK) {
        ESP_LOGE(TAG_STA, "NAPT not enabled on the netif: %p", esp_netif_ap);
    }

    /* ACL packet filter — install netif input/linkoutput hooks so the
     * four ACL chains (to_esp / from_esp / to_ap / from_ap) actually
     * drop denied traffic. Must run AFTER esp_wifi_start so the netifs
     * exist and have their default input/linkoutput function pointers. */
    netif_hooks_init();

    /* HTTP server with the embedded SPA at / + the JSON API endpoints. */
    web_ui_init();
}
