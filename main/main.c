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
#include "remote_console.h"
#include "nvs_params.h"
#include "pcap_capture.h"
#include "wifi_networks.h"
#include "dhcp_reservations.h"
#include "portmap.h"

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

/* Forward declarations — definitions land further down in this file. */
static void softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta);

/* Multi-network rotation state. Index 0 is preferred; on association
 * failure we tick s_net_retries and, after WIFI_RETRIES_PER_NETWORK,
 * roll forward to the next configured network. */
#define WIFI_RETRIES_PER_NETWORK 5
static int s_net_current = 0;
static int s_net_retries = 0;

/* Push a network entry (SSID/password + per-network static IP) into the
 * live esp_wifi + esp_netif config. Called once from wifi_init_sta()
 * with idx=0, then again from the STA_DISCONNECTED handler when the
 * rotation advances. */
static void wifi_apply_network(int idx)
{
    wifi_network_t n;
    if (!wifi_networks_get(idx, &n)) {
        ESP_LOGW("WiFi Sta", "wifi_apply_network: no network at idx %d", idx);
        return;
    }

    wifi_config_t cfg = {
        .sta = {
            .scan_method        = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt  = EXAMPLE_ESP_MAXIMUM_RETRY,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e        = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)cfg.sta.ssid,     n.ssid,   sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, n.passwd, sizeof cfg.sta.password);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    /* Apply per-network static IP. All three fields must be non-empty
     * for static mode; otherwise we revert to DHCP for this network. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dhcpc_stop(sta);
        if (n.static_ip[0] && n.subnet[0] && n.gateway[0]) {
            esp_netif_ip_info_t ip_info = {0};
            ip4_addr_t a;
            if (ip4addr_aton(n.static_ip, &a)) ip_info.ip.addr      = a.addr;
            if (ip4addr_aton(n.subnet,    &a)) ip_info.netmask.addr = a.addr;
            if (ip4addr_aton(n.gateway,   &a)) ip_info.gw.addr      = a.addr;
            esp_netif_set_ip_info(sta, &ip_info);

            /* Static mode disables the DHCP client, so no DNS lands on
             * the netif automatically. When the operator filled in the
             * per-network DNS field, use that; otherwise point DNS at
             * the gateway — most home routers act as a DNS forwarder,
             * which gives the same effective behaviour as DHCP would
             * have ("use whatever the upstream router uses"). */
            const char *dns_src = n.dns[0] ? n.dns : n.gateway;
            ip4_addr_t da;
            if (ip4addr_aton(dns_src, &da) && da.addr) {
                esp_netif_dns_info_t dns = {0};
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                dns.ip.u_addr.ip4.addr = da.addr;
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
            }

            ESP_LOGI("WiFi Sta", "wifi[%d] '%s' static %s/%s via %s dns=%s%s",
                     idx, n.ssid, n.static_ip, n.subnet, n.gateway, dns_src,
                     n.dns[0] ? "" : " (gateway)");
        } else {
            esp_netif_dhcpc_start(sta);
            ESP_LOGI("WiFi Sta", "wifi[%d] '%s' DHCP", idx, n.ssid);
        }
    }
}

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
        /* Multi-network rotation: stay on the current SSID for
         * WIFI_RETRIES_PER_NETWORK association attempts, then roll
         * forward to the next configured slot. Single-network setups
         * see no behaviour change because count<=1 skips the rotation. */
        int total = wifi_networks_count();
        if (total > 1) {
            s_net_retries++;
            if (s_net_retries >= WIFI_RETRIES_PER_NETWORK) {
                s_net_current = (s_net_current + 1) % total;
                s_net_retries = 0;
                ESP_LOGW(TAG_STA, "rotating to network[%d] of %d", s_net_current, total);
                wifi_apply_network(s_net_current);
            }
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_net_retries = 0;   /* successful association — clear the rotation counter */
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

        /* Copy the upstream DNS into the AP-side DHCP options so AP
         * clients can resolve names through us. Runs every time we
         * (re-)acquire an STA IP — DNS may have changed on the new
         * uplink, and DHCP server restart is idempotent. */
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (ap && sta) softap_set_dns_addr(ap, sta);

        /* Re-bind the NAPT portmap entries to the freshly-acquired
         * STA IP. portmap_install_all is idempotent — duplicate bindings
         * are cleared before the re-add. */
        portmap_install_all();
    }
}

/* Initialize soft AP. NVS-prefer: 'ap_ssid' / 'ap_passwd' / 'ap_channel'
 * from the project namespace, falling back to Kconfig only on first
 * boot before the operator runs the setup wizard. */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    /* Optional AP-side IP override. Empty NVS keys → keep the default
     * 192.168.4.1/24. When both are set and parse cleanly we stop the
     * DHCP server, re-install the IP/mask/gateway (gw == AP IP), then
     * restart DHCP so the new pool range is computed from the new IP. */
    char *nvs_ap_ip   = nvs_param_get_str("ap_ip");
    char *nvs_ap_mask = nvs_param_get_str("ap_mask");
    if (nvs_ap_ip && nvs_ap_ip[0] && nvs_ap_mask && nvs_ap_mask[0]) {
        ip4_addr_t ip_a, mask_a;
        if (ip4addr_aton(nvs_ap_ip,   &ip_a)
            && ip4addr_aton(nvs_ap_mask, &mask_a)
            && ip_a.addr != 0 && mask_a.addr != 0) {
            esp_netif_ip_info_t ip_info = {
                .ip      = { .addr = ip_a.addr },
                .netmask = { .addr = mask_a.addr },
                .gw      = { .addr = ip_a.addr },  /* AP is its own gw */
            };
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(esp_netif_ap));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_ip_info(esp_netif_ap, &ip_info));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(esp_netif_ap));
            ESP_LOGI(TAG_AP, "wifi_init_softap: custom AP %s/%s",
                     nvs_ap_ip, nvs_ap_mask);
        } else {
            ESP_LOGW(TAG_AP, "wifi_init_softap: bad ap_ip/ap_mask in NVS, using default");
        }
    }
    free(nvs_ap_ip);
    free(nvs_ap_mask);

    char *nvs_ssid = nvs_param_get_str("ap_ssid");
    char *nvs_pw   = nvs_param_get_str("ap_passwd");
    const char *use_ssid = (nvs_ssid && nvs_ssid[0]) ? nvs_ssid : EXAMPLE_ESP_WIFI_AP_SSID;
    const char *use_pw   = (nvs_pw   && nvs_pw[0])   ? nvs_pw   : EXAMPLE_ESP_WIFI_AP_PASSWD;
    int32_t channel = EXAMPLE_ESP_WIFI_CHANNEL;
    nvs_param_get_int("ap_channel", &channel);
    if (channel < 1 || channel > 13) channel = EXAMPLE_ESP_WIFI_CHANNEL;

    /* Hidden SSID — when set, the AP doesn't broadcast its name in
     * beacons. Clients still need the SSID to connect; this just stops
     * casual scanners from listing it. */
    uint8_t hidden = 0;
    nvs_param_get_u8("ap_hidden", &hidden);

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid_len = strlen(use_ssid),
            .channel = (uint8_t)channel,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
            .ssid_hidden = hidden ? 1 : 0,
        },
    };
    strlcpy((char*)wifi_ap_config.ap.ssid,     use_ssid, sizeof wifi_ap_config.ap.ssid);
    strlcpy((char*)wifi_ap_config.ap.password, use_pw,   sizeof wifi_ap_config.ap.password);
    if (use_pw[0] == '\0') wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_LOGI(TAG_AP, "wifi_init_softap: SSID '%s' channel %d (from %s)",
             use_ssid, (int)channel,
             (nvs_ssid && nvs_ssid[0]) ? "NVS" : "Kconfig");

    free(nvs_ssid);
    free(nvs_pw);
    return esp_netif_ap;
}

/* Initialize wifi station. Reads the network list (wifi_networks_init
 * migrated the legacy single-network keys into slot 0 on first boot),
 * applies slot 0, and falls back to the Kconfig defaults only when the
 * list is completely empty (genuinely fresh device). */
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    if (wifi_networks_count() > 0) {
        ESP_LOGI(TAG_STA, "wifi_init_sta: %d network(s) configured, starting with slot 0",
                 wifi_networks_count());
        s_net_current = 0;
        s_net_retries = 0;
        wifi_apply_network(0);
    } else {
        /* Our list is empty — but esp_wifi keeps its own NVS-backed
         * config under partition phy/nvs.net80211. Read it back and, if
         * a real SSID is already cached there, migrate it into our list
         * so we own it going forward. Otherwise fall through to the
         * Kconfig default. Never blindly overwrite a working config. */
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0]) {
            wifi_network_t n = {0};
            strlcpy(n.ssid,   (const char *)cur.sta.ssid,     sizeof n.ssid);
            strlcpy(n.passwd, (const char *)cur.sta.password, sizeof n.passwd);
            n.valid = 1;
            wifi_networks_set_all(&n, 1);
            ESP_LOGI(TAG_STA, "wifi_init_sta: migrated esp_wifi cached SSID '%s' into slot 0",
                     n.ssid);
        } else {
            wifi_config_t cfg = {
                .sta = {
                    .scan_method        = WIFI_ALL_CHANNEL_SCAN,
                    .failure_retry_cnt  = EXAMPLE_ESP_MAXIMUM_RETRY,
                    .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
                    .sae_pwe_h2e        = WPA3_SAE_PWE_BOTH,
                },
            };
            strlcpy((char *)cfg.sta.ssid,     EXAMPLE_ESP_WIFI_STA_SSID,     sizeof cfg.sta.ssid);
            strlcpy((char *)cfg.sta.password, EXAMPLE_ESP_WIFI_STA_PASSWD, sizeof cfg.sta.password);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
            ESP_LOGI(TAG_STA, "wifi_init_sta: no networks saved, using Kconfig default");
        }
    }

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

    /* Remote TCP REPL on a configurable port (default 2323). Starts the
     * listener only when previously enabled in NVS — disabled by default
     * for security. Auth-gated by the shared web_password hash. */
    remote_console_init();

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

    /* Bring the rest of the router up immediately. Earlier this code
     * blocked on xEventGroupWaitBits until the STA either connected or
     * gave up — but the AP-side services (web UI, NAPT, ACL hooks)
     * shouldn't depend on the uplink being up. The operator may need
     * the web UI precisely to configure the STA. The DNS-to-AP copy
     * has moved into the IP_EVENT_STA_GOT_IP handler so it still
     * runs whenever the uplink (re-)acquires an address. */

    /* STA is still the preferred default route for outgoing traffic
     * once it has an address; before that, the AP netif stays default. */
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

    /* Load the multi-network table BEFORE wifi_init_sta runs — that
     * function reads it to set up the initial association. The init
     * also migrates the legacy single-network NVS keys into slot 0
     * on first boot. */
    wifi_networks_init();

    /* DHCP reservation table — read now so the cached lookups are
     * ready before the AP netif starts handing out leases. The
     * matching DHCP-server hook lives in components/dhcpserver/. */
    dhcp_reservations_init();

    /* Port-forwarding (NAPT portmap) table — load from NVS now; the
     * actual lwIP bindings are installed from the IP_GOT_IP handler
     * once we know the STA's bind IP. */
    portmap_init();

    /* PCAP-over-TCP capture (listens on port 19000 for Wireshark).
     * Mode is OFF on boot; operator enables it from /api/tools/pcap. */
    pcap_init();

    /* HTTP server with the embedded SPA at / + the JSON API endpoints. */
    web_ui_init();
}
