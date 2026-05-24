/* Single-page web UI server.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "lwip/ip4_addr.h"
#include "web_ui.h"
#include "tailscale_config.h"
#include "tailscale_mtu.h"
#include "nvs_params.h"
#include "microlink.h"
#include "dns_relay.h"
#include "lwip_route_hook.h"
#include "acl.h"
#include "syslog_client.h"
#include "net_diag.h"
#include "pcap_capture.h"
#include "wifi_networks.h"
#include "dhcp_reservations.h"
#include "dhcps_ext.h"
#include "portmap.h"
#include "mac_deny.h"
#include "reset_history.h"
#include "https_cert.h"
#include "ota.h"
#include <stdlib.h>
#include <time.h>

/* Cap on log payloads we surface over /api endpoints — both the live
 * log tail and the pre-crash snapshot share this ceiling so the JSON
 * stays bounded. */
#define WEB_UI_LOG_SNAPSHOT_BYTES 4096
#include "telemetry.h"
#include "log_capture.h"
#include "netif_hooks.h"
#include "web_password.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_core_dump.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"

/* Globals owned by main.c — link status flags rendered in /api/status. */
extern int ap_connect;
extern int connect_count;

static const char *TAG = "web_ui";

/* index.html is generated as a gzipped C source by main/CMakeLists.txt
 * via the gen_index_html_gz.py helper. Stored compressed (≈5× smaller
 * than raw) and served with Content-Encoding: gzip — every modern
 * browser transparently inflates. See the build script for why the
 * Python-helper route was chosen over ESP-IDF EMBED_TXTFILES. */
extern const char   index_html_gz_start[];
extern const size_t index_html_gz_len;

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    /* Without this the browser happily reuses last session's SPA HTML
     * out of HTTP cache, so any client-side fix we ship requires the
     * operator to force-refresh before it takes effect. The SPA is
     * tiny and served from RAM — no reason to cache. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, index_html_gz_start, index_html_gz_len);
}

static const httpd_uri_t uri_index = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_handler,
    .user_ctx = NULL,
};

static void ip4_to_str(uint32_t ip_nbo, char *out, size_t out_size)
{
    const ip4_addr_t a = { .addr = ip_nbo };
    snprintf(out, out_size, IPSTR, IP2STR(&a));
}

/* Forward decls — definitions live further down. */
static bool  request_authenticated(httpd_req_t *req);
static void  ip4_hbo_to_str(uint32_t hbo, char *out, size_t out_size);
static char *device_name_dup(void);

static esp_err_t require_auth(httpd_req_t *req)
{
    if (request_authenticated(req)) return ESP_OK;
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "auth required");
    return ESP_FAIL;
}

/* Translate esp_reset_reason() into the short string the SPA renders.
 * IDF 5.5 added USB / JTAG / EFUSE / PWR_GLITCH / CPU_LOCKUP — without
 * those cases the switch fell through to UNKNOWN every time the S3
 * came back from a clean esp_restart(), since IDF reports it as one
 * of the new codes rather than the legacy ESP_RST_SW. */
static const char *reset_reason_str(void)
{
    /* ESP_RST_* are enum values (not #defines), so the previous #ifdef
     * guards never compiled the new IDF 5.x cases in — that's why the
     * S3 always reported UNKNOWN after a clean esp_restart(). With the
     * guards gone, the chip's actual cause shows through. */
    esp_reset_reason_t r = esp_reset_reason();
    static char unk[16];
    switch (r) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:
            /* Surface the raw enum code so future IDF additions show up. */
            snprintf(unk, sizeof unk, "RAW_%d", (int)r);
            return unk;
    }
}

/* Live CPU-load percentage from the FreeRTOS runtime-stats counters.
 * Compares the IDLE0+IDLE1 task tick deltas against total task ticks
 * since the previous sample, so the first call returns 0 and every
 * call after that gives the load over the elapsed interval. Throttled
 * to 1-per-second so the same /api/status poll burst doesn't churn
 * the sampler. Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y. */
static uint8_t sample_cpu_load_pct(void)
{
    static uint64_t s_last_sample_us = 0;
    static uint32_t s_last_total     = 0;
    static uint32_t s_last_idle      = 0;
    static uint8_t  s_last_load_pct  = 0;

    uint64_t now = (uint64_t)esp_timer_get_time();
    if (s_last_sample_us != 0 && now - s_last_sample_us < 1000000) {
        return s_last_load_pct;
    }

    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = malloc(sizeof(TaskStatus_t) * n);
    if (!arr) return s_last_load_pct;
    uint32_t total = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n, &total);

    uint32_t idle = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        if (arr[i].pcTaskName && strncmp(arr[i].pcTaskName, "IDLE", 4) == 0) {
            idle += arr[i].ulRunTimeCounter;
        }
    }
    free(arr);

    if (s_last_sample_us != 0 && total > s_last_total) {
        uint32_t total_delta = total - s_last_total;
        uint32_t idle_delta  = idle  - s_last_idle;
        if (idle_delta >= total_delta) {
            s_last_load_pct = 0;
        } else {
            s_last_load_pct = (uint8_t)(100 - ((uint64_t)idle_delta * 100 / total_delta));
        }
    }
    s_last_total     = total;
    s_last_idle      = idle;
    s_last_sample_us = now;
    return s_last_load_pct;
}

/* Internal CPU temperature in °C, or -999 if the sensor isn't available.
 * Lazy-installs on first call; the sensor draws ~1 mA continuously while
 * enabled, so we keep it running once installed rather than turn it on
 * and off per sample. Diag-tab also calls into this via its own copy
 * (kept until the diag handler migrates to this shared helper). */
static float sample_cpu_temp_c(void)
{
    static temperature_sensor_handle_t s_sensor = NULL;
    if (!s_sensor) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_sensor) != ESP_OK) return -999.0f;
        temperature_sensor_enable(s_sensor);
    }
    float tc = 0;
    if (temperature_sensor_get_celsius(s_sensor, &tc) != ESP_OK) return -999.0f;
    return tc;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version",      desc ? desc->version : "?");
    cJSON_AddNumberToObject(root, "uptime_s",     esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap",    esp_get_free_heap_size());
    /* Heap total tracks the largest-known-free moment since boot; the
     * SPA only uses it to draw the % bar, so the exact denominator
     * isn't important — what matters is that it stays >= free_heap. */
    cJSON_AddNumberToObject(root, "heap_total",   heap_caps_get_total_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "free_psram",   heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* Per-heap split for the Status System tile — separate progress bars
     * for internal DRAM vs SPIRAM let the operator see which heap is
     * actually under pressure (internal is the constrained one). */
    cJSON_AddNumberToObject(root, "mem_internal_free",  heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "mem_internal_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "mem_spiram_free",    heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "mem_spiram_total",   heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "cpu_load_pct",       sample_cpu_load_pct());
    {
        float tc = sample_cpu_temp_c();
        if (tc > -100.0f) cJSON_AddNumberToObject(root, "cpu_temp_c", tc);
    }
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str());

    /* STA (uplink) — SSID, IP, RSSI, MAC. */
    cJSON *sta = cJSON_CreateObject();
    cJSON_AddBoolToObject(sta, "connected", ap_connect != 0);
    wifi_ap_record_t apr;
    if (ap_connect && esp_wifi_sta_get_ap_info(&apr) == ESP_OK) {
        cJSON_AddStringToObject(sta, "ssid", (const char *)apr.ssid);
        cJSON_AddNumberToObject(sta, "rssi", apr.rssi);
        cJSON_AddNumberToObject(sta, "channel", apr.primary);
    }
    uint8_t mac[6];
    char mac_str[18];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(sta, "mac", mac_str);
    }
    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta_if && esp_netif_get_ip_info(sta_if, &ip) == ESP_OK) {
        char buf[16];
        ip4_to_str(ip.ip.addr, buf, sizeof buf);
        cJSON_AddStringToObject(sta, "ip", buf);
        ip4_to_str(ip.gw.addr, buf, sizeof buf);
        cJSON_AddStringToObject(sta, "gateway", buf);
    }
    /* DNS — main resolver only; secondary is rarely set on this device.
     * Reading via esp_netif_get_dns_info so we don't have to track
     * whether DHCP or our static override last touched the slot. */
    if (sta_if) {
        esp_netif_dns_info_t dns_info = {0};
        if (esp_netif_get_dns_info(sta_if, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK
            && dns_info.ip.u_addr.ip4.addr) {
            char buf[16];
            ip4_to_str(dns_info.ip.u_addr.ip4.addr, buf, sizeof buf);
            cJSON_AddStringToObject(sta, "dns", buf);
        }
    }
    cJSON_AddNumberToObject(sta, "bytes_in",  (double)netif_hooks_get_sta_bytes_in());
    cJSON_AddNumberToObject(sta, "bytes_out", (double)netif_hooks_get_sta_bytes_out());
    cJSON_AddItemToObject(root, "sta", sta);

    /* AP (downlink) — SSID + channel from live wifi_config, MAC, clients, IP. */
    cJSON *ap = cJSON_CreateObject();
    cJSON_AddNumberToObject(ap, "clients", connect_count);
    wifi_config_t ap_cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
        cJSON_AddStringToObject(ap, "ssid",    (const char *)ap_cfg.ap.ssid);
        cJSON_AddNumberToObject(ap, "channel", ap_cfg.ap.channel);
    }
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(ap, "mac", mac_str);
    }
    esp_netif_t *ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_if && esp_netif_get_ip_info(ap_if, &ip) == ESP_OK) {
        char buf[16];
        ip4_to_str(ip.ip.addr, buf, sizeof buf);
        cJSON_AddStringToObject(ap, "ip", buf);
    }
    cJSON_AddNumberToObject(ap, "bytes_in",  (double)netif_hooks_get_ap_bytes_in());
    cJSON_AddNumberToObject(ap, "bytes_out", (double)netif_hooks_get_ap_bytes_out());
    cJSON_AddItemToObject(root, "ap", ap);

    /* Radio-wide TX power (live, post-override). Same value for both
     * STA + AP — kept at the root rather than duplicated under each
     * interface object since it's a single radio setting. */
    {
        int8_t live_pwr = 0;
        if (esp_wifi_get_max_tx_power(&live_pwr) == ESP_OK) {
            cJSON_AddNumberToObject(root, "tx_power", live_pwr);
        }
    }

    /* Tailscale (microlink) — runtime state from tailscale_config.h.
     * tailscale_is_connected() polls microlink + refreshes the cached
     * tunnel_ip; we use its return value over the stale bool global. */
    bool ts_connected = tailscale_is_connected();
    cJSON *ts = cJSON_CreateObject();
    cJSON_AddBoolToObject  (ts, "enabled",   tailscale_enabled != 0);
    cJSON_AddBoolToObject  (ts, "connected", ts_connected);
    if (tailscale_hostname)         cJSON_AddStringToObject(ts, "hostname",         tailscale_hostname);
    if (tailscale_advertise_routes) cJSON_AddStringToObject(ts, "advertise_routes", tailscale_advertise_routes);
    if (tailscale_tunnel_ip) {
        char buf[16];
        ip4_to_str(tailscale_tunnel_ip, buf, sizeof buf);
        cJSON_AddStringToObject(ts, "tunnel_ip", buf);
    }
    if (tailscale_exit_node_ip) {
        char buf[16];
        ip4_hbo_to_str(tailscale_exit_node_ip, buf, sizeof buf);
        cJSON_AddStringToObject(ts, "exit_node_ip", buf);
    }
    /* Peer count summary — full peer table lives at /api/tailscale.
     * While we are still in Registering state, microlink_peer_info_t.online
     * reflects "control-plane echoed this peer" rather than "we have a
     * verified session" — so it briefly reports everyone online + DERP
     * before DISCO settles. Suppress that to avoid lying to the SPA. */
    int online = 0, total = 0;
    struct microlink_s *ml = tailscale_get_microlink();
    if (ml) {
        total = microlink_get_peer_count(ml);
        if (ts_connected) {
            for (int i = 0; i < total; i++) {
                microlink_peer_info_t pi;
                if (microlink_get_peer_info(ml, i, &pi) == ESP_OK && pi.online) online++;
            }
        }
    }
    cJSON_AddNumberToObject(ts, "peers_online", online);
    cJSON_AddNumberToObject(ts, "peers_total",  total);
    /* Auth-failure surface — same field the Tailscale tab reads; the
     * Status page consumes it for the small TS badge in the AP card. */
    if (ml) {
        microlink_diag_t diag;
        if (microlink_get_diag(ml, &diag) == ESP_OK) {
            cJSON_AddNumberToObject(ts, "register_user_id", diag.register_user_id);
            if (diag.register_user_name[0]) {
                cJSON_AddStringToObject(ts, "register_user_name", diag.register_user_name);
            }
            cJSON_AddBoolToObject  (ts, "identity_persistent", diag.identity_persistent);
            cJSON_AddStringToObject(ts, "identity_pubkey_prefix", diag.identity_pubkey_prefix);
        }
    }
    cJSON_AddItemToObject(root, "tailscale", ts);

    /* Telemetry summary — full counters in /api/system. */
    telemetry_state_t tm = telemetry_get_state();
    cJSON *tlm = cJSON_CreateObject();
    cJSON_AddStringToObject(tlm, "status", tm.enabled ? "ok" : "off");
    cJSON_AddNumberToObject(tlm, "boot_count",  tm.boot_count);
    cJSON_AddNumberToObject(tlm, "flash_count", tm.flash_count);
    cJSON_AddItemToObject(root, "telemetry", tlm);

    /* OTA banner hint — minimal subset of /api/system ota{} so the
     * Status page banner can render without a second round-trip. */
    {
        ota_state_t os;
        ota_get_state(&os);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject  (o, "update_available", os.update_available);
        cJSON_AddBoolToObject  (o, "auto_install",     os.auto_install);
        cJSON_AddNumberToObject(o, "install_hour",     os.install_hour);
        cJSON_AddStringToObject(o, "latest_version",   os.last_version);
        cJSON_AddStringToObject(o, "running_version",  os.running_version);
        cJSON_AddItemToObject(root, "ota", o);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_status = {
    .uri      = "/api/status",
    .method   = HTTP_GET,
    .handler  = status_handler,
    .user_ctx = NULL,
};

/* Helper: read NVS string and attach to obj if non-empty. Frees the buffer. */
static void add_nvs_string(cJSON *obj, const char *json_key, const char *nvs_key)
{
    char *s = nvs_param_get_str(nvs_key);
    if (s) {
        if (s[0]) cJSON_AddStringToObject(obj, json_key, s);
        free(s);
    }
}

static esp_err_t network_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* networks[] — priority-ordered uplink list. Passwords are NEVER
     * serialised. Static IP block is included as a sub-object per
     * network; empty fields mean DHCP for that entry. */
    cJSON *nets = cJSON_CreateArray();
    int count = wifi_networks_count();
    for (int i = 0; i < count; i++) {
        wifi_network_t n;
        if (!wifi_networks_get(i, &n)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", n.ssid);
        cJSON *sip = cJSON_CreateObject();
        if (n.static_ip[0]) cJSON_AddStringToObject(sip, "ip",   n.static_ip);
        if (n.subnet[0])    cJSON_AddStringToObject(sip, "mask", n.subnet);
        if (n.gateway[0])   cJSON_AddStringToObject(sip, "gw",   n.gateway);
        if (n.dns[0])       cJSON_AddStringToObject(sip, "dns",  n.dns);
        cJSON_AddItemToObject(e, "static_ip", sip);
        /* EAP / WPA2-Enterprise block — password is NEVER emitted; identity
         * and username are because the SPA needs them to render the row
         * and the operator should be able to see them at a glance. */
        cJSON *eap = cJSON_CreateObject();
        cJSON_AddNumberToObject(eap, "method", n.eap_method);
        cJSON_AddNumberToObject(eap, "phase2", n.eap_phase2);
        cJSON_AddBoolToObject  (eap, "cert_bundle", n.eap_use_cert_bundle != 0);
        cJSON_AddStringToObject(eap, "identity", n.eap_identity);
        cJSON_AddStringToObject(eap, "username", n.eap_username);
        cJSON_AddBoolToObject  (eap, "has_password", n.eap_password[0] != '\0');
        cJSON_AddItemToObject(e, "eap", eap);
        cJSON_AddItemToArray(nets, e);
    }
    cJSON_AddItemToObject(root, "networks", nets);

    /* Hostname is a device-wide setting (not per-network). */
    add_nvs_string(root, "hostname", "hostname");

    /* STA TTL hop-limit override — 0 means passthrough (no rewrite),
     * non-zero is the value the netif hook stamps on every outgoing
     * IPv4 frame. Device-wide; lives next to hostname in the JSON. */
    {
        uint8_t ttl = netif_hooks_get_sta_ttl();
        cJSON_AddNumberToObject(root, "sta_ttl_override", ttl);
    }

    /* AP — same omit-rule on the password. */
    cJSON *ap = cJSON_CreateObject();
    add_nvs_string(ap, "ssid", "ap_ssid");
    int32_t channel = 0;
    if (nvs_param_get_int("ap_channel", &channel) == ESP_OK && channel > 0) {
        cJSON_AddNumberToObject(ap, "channel", channel);
    }
    /* AP-side IP override — empty / unset means the default 192.168.4.1/24. */
    add_nvs_string(ap, "ip",   "ap_ip");
    add_nvs_string(ap, "mask", "ap_mask");
    add_nvs_string(ap, "dns",  "ap_dns");
    uint8_t ap_hidden = 0;
    nvs_param_get_u8("ap_hidden", &ap_hidden);
    cJSON_AddBoolToObject(ap, "hidden", ap_hidden != 0);

    /* DNS relay state — the "AP clients see ESP as resolver" mode. */
    {
        cJSON *dr = cJSON_CreateObject();
        cJSON_AddBoolToObject(dr, "enabled",  dns_relay_is_enabled());
        cJSON_AddBoolToObject(dr, "healthy",  dns_relay_is_healthy());
        uint32_t up_nbo = dns_relay_get_upstream();
        if (up_nbo) {
            char buf[16];
            snprintf(buf, sizeof buf, "%u.%u.%u.%u",
                     (unsigned)( up_nbo        & 0xff),
                     (unsigned)((up_nbo >>  8) & 0xff),
                     (unsigned)((up_nbo >> 16) & 0xff),
                     (unsigned)((up_nbo >> 24) & 0xff));
            cJSON_AddStringToObject(dr, "upstream", buf);
        } else {
            cJSON_AddStringToObject(dr, "upstream", "");
        }
        cJSON_AddItemToObject(ap, "dns_relay", dr);
    }
    cJSON_AddItemToObject(root, "ap", ap);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_network = {
    .uri      = "/api/network",
    .method   = HTTP_GET,
    .handler  = network_handler,
    .user_ctx = NULL,
};

/* Read up to (buf_size - 1) bytes of the POST body into buf and NUL-
 * terminate. Returns ESP_OK on success or after sending the appropriate
 * error response on failure. */
/* SPIRAM-first body-buffer allocator for the save-handlers' 2-4 KB
 * scratch space. The IDF default CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=
 * 16384 would otherwise route every sub-16K malloc() to the much
 * smaller internal DRAM heap (~346 KB total, ~41 KB free under load).
 * Explicit MALLOC_CAP_SPIRAM keeps the buffers on the 8 MB SPIRAM
 * heap where there's effectively unlimited room. Returns NULL when
 * SPIRAM is exhausted — caller already has the NULL-check path. */
static inline char *malloc_body_buf(size_t n) {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size, int *out_len)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return ESP_FAIL;
    }
    int n = httpd_req_recv(req, buf, req->content_len);
    if (n <= 0) {
        if (n == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return ESP_OK;
}

/* Write an NVS string key only when the JSON object carries a string
 * value for the given json_key. Empty string is accepted as "clear". */
/* Per-request NVS-error capture. Each save_*_if_present helper sets
 * this when nvs_param_set_* returns non-OK; the surrounding handler
 * checks it before claiming success. Reset at handler entry via
 * nvs_save_errors_reset(). Single-threaded — httpd runs save handlers
 * one at a time on its worker task so a static is fine. */
static struct {
    int       count;
    esp_err_t first_err;
    char      first_key[24];
} s_save_err = { 0 };

static void nvs_save_errors_reset(void) {
    s_save_err.count = 0;
    s_save_err.first_err = ESP_OK;
    s_save_err.first_key[0] = '\0';
}

static void nvs_save_record_err(const char *nvs_key, esp_err_t err) {
    if (err == ESP_OK) return;
    s_save_err.count++;
    if (s_save_err.first_err == ESP_OK) {
        s_save_err.first_err = err;
        strlcpy(s_save_err.first_key, nvs_key, sizeof s_save_err.first_key);
    }
}

/* Shorthand for the handlers that don't carry extra fields in the
 * response: build {ok, [nvs_save_error]} from the accumulated error
 * state and send it. Returns the httpd send-result so the handler
 * can pass it straight through. */
static esp_err_t send_save_response(httpd_req_t *req);

/* Attach `ok` + optional `nvs_save_error` block to a response body
 * cJSON object based on the accumulated per-request error state. The
 * SPA renders nvs_save_error as a red toast — operator sees the
 * silent NVS-out-of-space failure instead of a green "Saved" lie. */
static void nvs_save_errors_attach(cJSON *root) {
    bool ok = (s_save_err.count == 0);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "count",     s_save_err.count);
        cJSON_AddStringToObject(e, "first_key", s_save_err.first_key);
        cJSON_AddStringToObject(e, "first_err", esp_err_to_name(s_save_err.first_err));
        cJSON_AddItemToObject(root, "nvs_save_error", e);
    }
}

static esp_err_t send_save_response(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    nvs_save_errors_attach(resp);
    char *body = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{\"ok\":true}");
    free(body);
    return e;
}

/* Thin wrappers that route every NVS write through the per-request
 * error tracker. Replaces direct nvs_param_set_*() calls inside save
 * handlers — operator gets a real toast instead of a green "Saved"
 * lie when an out-of-space write silently fails. */
static void nvs_save_str(const char *key, const char *value) {
    esp_err_t err = nvs_param_set_str(key, value ? value : "");
    if (err != ESP_OK) {
        ESP_LOGE("web_ui", "NVS write FAILED for key=%s (str len=%u): %s",
                 key, (unsigned)(value ? strlen(value) : 0), esp_err_to_name(err));
        nvs_save_record_err(key, err);
    }
}
static void nvs_save_int(const char *key, int32_t value) {
    esp_err_t err = nvs_param_set_int(key, value);
    if (err != ESP_OK) {
        ESP_LOGE("web_ui", "NVS write FAILED for key=%s (int=%ld): %s",
                 key, (long)value, esp_err_to_name(err));
        nvs_save_record_err(key, err);
    }
}
static void nvs_save_u8(const char *key, uint8_t value) {
    esp_err_t err = nvs_param_set_u8(key, value);
    if (err != ESP_OK) {
        ESP_LOGE("web_ui", "NVS write FAILED for key=%s (u8=%u): %s",
                 key, (unsigned)value, esp_err_to_name(err));
        nvs_save_record_err(key, err);
    }
}
static void nvs_save_u32(const char *key, uint32_t value) {
    esp_err_t err = nvs_param_set_u32(key, value);
    if (err != ESP_OK) {
        ESP_LOGE("web_ui", "NVS write FAILED for key=%s (u32=%lu): %s",
                 key, (unsigned long)value, esp_err_to_name(err));
        nvs_save_record_err(key, err);
    }
}

static void save_str_if_present(const cJSON *obj, const char *json_key, const char *nvs_key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, json_key);
    if (cJSON_IsString(v)) nvs_save_str(nvs_key, v->valuestring);
}

static void save_int_if_present(const cJSON *obj, const char *json_key, const char *nvs_key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, json_key);
    if (cJSON_IsNumber(v)) nvs_save_int(nvs_key, (int32_t)v->valuedouble);
}

/* Look up the saved password for an SSID we already know about. Used
 * to honour omit-to-keep when the SPA sends an entry without a
 * password field (or with an empty one). Returns true if found. */
static bool lookup_existing_password(const char *ssid, char *out, size_t out_size)
{
    int count = wifi_networks_count();
    for (int j = 0; j < count; j++) {
        wifi_network_t n;
        if (wifi_networks_get(j, &n) && strcmp(n.ssid, ssid) == 0) {
            strlcpy(out, n.passwd, out_size);
            return true;
        }
    }
    return false;
}

/* Same omit-to-keep pattern for the EAP inner password — we never echo
 * it in the GET response (security), so the SPA can't round-trip it on
 * save. When the POST body omits eap_password (or sends an empty one)
 * AND the SSID already exists with a non-empty stored credential, copy
 * it across; otherwise the operator is providing a fresh credential. */
static bool lookup_existing_eap_password(const char *ssid, char *out, size_t out_size)
{
    int count = wifi_networks_count();
    for (int j = 0; j < count; j++) {
        wifi_network_t n;
        if (wifi_networks_get(j, &n) && strcmp(n.ssid, ssid) == 0) {
            strlcpy(out, n.eap_password, out_size);
            return true;
        }
    }
    return false;
}

/* Count leading 1-bits in a host-byte-order subnet mask. Returns -1
 * if the mask is non-contiguous (which lwIP rejects anyway). */
static int subnet_mask_prefix_len(uint32_t mask_nbo)
{
    uint32_t bits = ntohl(mask_nbo);
    int prefix = 0;
    while (bits & 0x80000000u) { prefix++; bits <<= 1; }
    return bits ? -1 : prefix;
}

/* Read NVS ap_ip / ap_mask (falling back to the firmware default
 * 192.168.4.0/24 when either is empty) and write the resulting CIDR
 * string "a.b.c.d/N" into `out`. Used as a stable identifier of the
 * AP-side subnet for the advertised-routes auto-maintenance. */
static void compute_ap_cidr_from_nvs(char *out, size_t out_size)
{
    const char *def = "192.168.4.0/24";
    char *ip_str   = nvs_param_get_str("ap_ip");
    char *mask_str = nvs_param_get_str("ap_mask");
    ip4_addr_t ip = {0}, mask = {0};
    int prefix = -1;
    if (ip_str && ip_str[0] && mask_str && mask_str[0]
        && ip4addr_aton(ip_str,   &ip)
        && ip4addr_aton(mask_str, &mask)
        && (prefix = subnet_mask_prefix_len(mask.addr)) >= 0) {
        ip4_addr_t net = { .addr = ip.addr & mask.addr };
        char buf[16];
        snprintf(buf, sizeof buf, IPSTR, IP2STR(&net));
        /* Cast to unsigned + %u so GCC's snprintf size analysis sees
         * a 0-32 range instead of the full int(11+sign) worst-case. */
        snprintf(out, out_size, "%s/%u", buf, (unsigned)prefix);
    } else {
        strlcpy(out, def, out_size);
    }
    free(ip_str);
    free(mask_str);
}

/* Inspect ts_advertise_routes against the AP CIDR change and propose
 * what the new value should be, WITHOUT touching NVS. The UI prompts
 * the operator and POSTs the proposed string back via /api/tailscale.
 *
 * Cases:
 *   * Routes empty → propose "new_cidr" (first-time fill).
 *   * Old CIDR in routes → propose old replaced by new.
 *   * Old CIDR absent → propose appending new_cidr.
 *   * new_cidr already there + no old to drop → nothing to propose.
 *
 * Operator-added custom routes are preserved in the proposal.
 * Returns true when a proposal exists; out_proposed_routes is filled
 * only in that case. */
static bool maintain_ap_cidr_in_routes(const char *old_cidr,
                                       const char *new_cidr,
                                       bool *out_offer_add,
                                       char *out_proposed_routes,
                                       size_t proposed_size)
{
    if (out_offer_add) *out_offer_add = false;
    if (!new_cidr || !*new_cidr) return false;

    /* NVS keys are limited to 15 chars — the rest of the code base
     * stores this value under "ts_routes" (matches tailscale_init and
     * tailscale_save_handler), so the auto-maintain has to use the
     * same key or the write lands on a different (and silently
     * rejected) slot. */
    char *routes = nvs_param_get_str("ts_routes");
    const bool routes_empty = !routes || !routes[0];
    const bool unchanged = old_cidr && strcmp(old_cidr, new_cidr) == 0;

    /* First pass: track presence of old / new in the existing routes. */
    bool old_present = false, new_present = false;
    if (!routes_empty) {
        const char *p = routes;
        while (*p) {
            const char *eol = p;
            while (*eol && *eol != '\n' && *eol != '\r') eol++;
            size_t llen = (size_t)(eol - p);
            while (llen > 0 && (p[llen - 1] == ' ' || p[llen - 1] == '\t')) llen--;
            if (llen > 0) {
                if (old_cidr && strlen(old_cidr) == llen
                    && strncmp(p, old_cidr, llen) == 0) old_present = true;
                if (strlen(new_cidr) == llen
                    && strncmp(p, new_cidr, llen) == 0) new_present = true;
            }
            p = eol;
            while (*p == '\n' || *p == '\r') p++;
        }
    }

    /* Build the *proposed* updated routes: drop old_cidr lines when
     * different from new, then ensure new_cidr is present once. */
    char out[512];
    out[0] = '\0';
    bool out_has_new = false;
    if (!routes_empty) {
        const char *p = routes;
        while (*p) {
            const char *eol = p;
            while (*eol && *eol != '\n' && *eol != '\r') eol++;
            size_t llen = (size_t)(eol - p);
            while (llen > 0 && (p[llen - 1] == ' ' || p[llen - 1] == '\t')) llen--;
            if (llen > 0) {
                bool is_old = !unchanged && old_cidr
                              && strlen(old_cidr) == llen
                              && strncmp(p, old_cidr, llen) == 0;
                bool is_new = strlen(new_cidr) == llen
                              && strncmp(p, new_cidr, llen) == 0;
                if (is_new && out_has_new) {
                    /* Duplicate of new_cidr — drop. */
                } else if (!is_old) {
                    if (out[0]) strlcat(out, "\n", sizeof out);
                    strncat(out, p, llen);
                    if (is_new) out_has_new = true;
                }
            }
            p = eol;
            while (*p == '\n' || *p == '\r') p++;
        }
    }
    if (!out_has_new) {
        if (out[0]) strlcat(out, "\n", sizeof out);
        strlcat(out, new_cidr, sizeof out);
    }

    /* If the proposed string matches what's already in NVS, there's
     * nothing to ask — just bail. Otherwise hand the proposed string
     * back to the caller; we never touch NVS ourselves now, the SPA
     * confirms with the operator and POSTs the change via
     * /api/tailscale. */
    bool would_change = !routes || strcmp(routes, out) != 0;
    free(routes);
    if (!would_change) return false;

    if (out_offer_add) *out_offer_add = true;
    if (out_proposed_routes && proposed_size > 0) {
        strlcpy(out_proposed_routes, out, proposed_size);
    }
    ESP_LOGI(TAG, "ts_routes: AP CIDR %s%s%s — offering UI update",
             old_cidr ? old_cidr : "(none)",
             (old_cidr && new_cidr) ? " → " : "",
             new_cidr);
    (void)old_present; (void)new_present; (void)unchanged;
    return true;
}

static esp_err_t network_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    nvs_save_errors_reset();

    /* Heap-allocate the body buffer — a 4 KB stack-local on the httpd
     * worker overflows the task stack once cJSON parsing piles on. */
    size_t buf_size = 4096;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* networks[] — preferred shape. Replaces the WHOLE list. Each entry
     * may omit `password` (or leave it empty) to keep the stored
     * credential — matched by SSID against the existing table. */
    cJSON *nets_j = cJSON_GetObjectItem(root, "networks");
    if (cJSON_IsArray(nets_j)) {
        wifi_network_t arr[WIFI_NETWORKS_MAX];
        memset(arr, 0, sizeof arr);
        int n_in    = cJSON_GetArraySize(nets_j);
        int n_out   = 0;
        for (int i = 0; i < n_in && n_out < WIFI_NETWORKS_MAX; i++) {
            cJSON *e = cJSON_GetArrayItem(nets_j, i);
            if (!cJSON_IsObject(e)) continue;
            cJSON *ssid_j = cJSON_GetObjectItem(e, "ssid");
            if (!cJSON_IsString(ssid_j) || !ssid_j->valuestring[0]) continue;

            wifi_network_t *n = &arr[n_out];
            strlcpy(n->ssid, ssid_j->valuestring, sizeof n->ssid);

            cJSON *pw_j = cJSON_GetObjectItem(e, "password");
            if (cJSON_IsString(pw_j) && pw_j->valuestring[0]) {
                strlcpy(n->passwd, pw_j->valuestring, sizeof n->passwd);
            } else {
                lookup_existing_password(n->ssid, n->passwd, sizeof n->passwd);
            }

            cJSON *sip = cJSON_GetObjectItem(e, "static_ip");
            if (cJSON_IsObject(sip)) {
                const cJSON *ip   = cJSON_GetObjectItem(sip, "ip");
                const cJSON *mask = cJSON_GetObjectItem(sip, "mask");
                const cJSON *gw   = cJSON_GetObjectItem(sip, "gw");
                const cJSON *dns  = cJSON_GetObjectItem(sip, "dns");
                if (cJSON_IsString(ip))   strlcpy(n->static_ip, ip->valuestring,   sizeof n->static_ip);
                if (cJSON_IsString(mask)) strlcpy(n->subnet,    mask->valuestring, sizeof n->subnet);
                if (cJSON_IsString(gw))   strlcpy(n->gateway,   gw->valuestring,   sizeof n->gateway);
                if (cJSON_IsString(dns))  strlcpy(n->dns,       dns->valuestring,  sizeof n->dns);
            }

            /* EAP block — every field is optional. method=0 (DISABLED)
             * keeps the plain-PSK behaviour. eap_password follows the
             * same omit-to-keep pattern as the regular PSK password. */
            cJSON *eap = cJSON_GetObjectItem(e, "eap");
            if (cJSON_IsObject(eap)) {
                const cJSON *m  = cJSON_GetObjectItem(eap, "method");
                const cJSON *p2 = cJSON_GetObjectItem(eap, "phase2");
                const cJSON *cb = cJSON_GetObjectItem(eap, "cert_bundle");
                const cJSON *id = cJSON_GetObjectItem(eap, "identity");
                const cJSON *un = cJSON_GetObjectItem(eap, "username");
                const cJSON *pw = cJSON_GetObjectItem(eap, "password");
                if (cJSON_IsNumber(m))  n->eap_method = (uint8_t)m->valueint;
                if (cJSON_IsNumber(p2)) n->eap_phase2 = (uint8_t)p2->valueint;
                if (cJSON_IsBool(cb))   n->eap_use_cert_bundle = cJSON_IsTrue(cb) ? 1 : 0;
                if (cJSON_IsString(id)) strlcpy(n->eap_identity, id->valuestring, sizeof n->eap_identity);
                if (cJSON_IsString(un)) strlcpy(n->eap_username, un->valuestring, sizeof n->eap_username);
                if (cJSON_IsString(pw) && pw->valuestring[0]) {
                    strlcpy(n->eap_password, pw->valuestring, sizeof n->eap_password);
                } else {
                    lookup_existing_eap_password(n->ssid, n->eap_password, sizeof n->eap_password);
                }
            }

            n->valid = 1;
            n_out++;
        }
        wifi_networks_set_all(arr, n_out);
    }

    /* Backward-compat path: pre-multi-network SPA clients still POST
     * { sta:{ ssid, password, hostname }, static_ip:{...} } — when
     * they do, treat it as a single-entry write to slot 0. */
    cJSON *sta = cJSON_GetObjectItem(root, "sta");
    if (cJSON_IsObject(sta) && !cJSON_IsArray(nets_j)) {
        wifi_network_t one = {0};
        const cJSON *ssid_j = cJSON_GetObjectItem(sta, "ssid");
        if (cJSON_IsString(ssid_j) && ssid_j->valuestring[0]) {
            strlcpy(one.ssid, ssid_j->valuestring, sizeof one.ssid);
            const cJSON *pw_j = cJSON_GetObjectItem(sta, "password");
            if (cJSON_IsString(pw_j) && pw_j->valuestring[0]) {
                strlcpy(one.passwd, pw_j->valuestring, sizeof one.passwd);
            } else {
                lookup_existing_password(one.ssid, one.passwd, sizeof one.passwd);
            }
            cJSON *sip = cJSON_GetObjectItem(root, "static_ip");
            if (cJSON_IsObject(sip)) {
                const cJSON *ip   = cJSON_GetObjectItem(sip, "ip");
                const cJSON *mask = cJSON_GetObjectItem(sip, "mask");
                const cJSON *gw   = cJSON_GetObjectItem(sip, "gw");
                const cJSON *dns  = cJSON_GetObjectItem(sip, "dns");
                if (cJSON_IsString(ip))   strlcpy(one.static_ip, ip->valuestring,   sizeof one.static_ip);
                if (cJSON_IsString(mask)) strlcpy(one.subnet,    mask->valuestring, sizeof one.subnet);
                if (cJSON_IsString(gw))   strlcpy(one.gateway,   gw->valuestring,   sizeof one.gateway);
                if (cJSON_IsString(dns))  strlcpy(one.dns,       dns->valuestring,  sizeof one.dns);
            }
            one.valid = 1;
            wifi_networks_set_all(&one, 1);
        }
        save_str_if_present(sta, "hostname", "hostname");
    }

    /* Hostname (device-wide) and AP block are still saved via the same
     * legacy NVS keys regardless of which write shape the client used. */
    save_str_if_present(root, "hostname", "hostname");

    /* STA TTL override — clamp to 0..255 (u8) and apply LIVE so the
     * change takes effect on the next outgoing IPv4 frame without
     * waiting for a reboot. */
    const cJSON *ttl_j = cJSON_GetObjectItem(root, "sta_ttl_override");
    if (cJSON_IsNumber(ttl_j)) {
        int v = (int)ttl_j->valuedouble;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        nvs_save_u8("sta_ttl", (uint8_t)v);
        netif_hooks_set_sta_ttl((uint8_t)v);
    }

    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    if (cJSON_IsObject(ap)) {
        /* Snapshot the AP CIDR BEFORE the save so we can swap it out of
         * ts_advertise_routes after — keeps operator-added manual
         * routes intact and just maintains the auto-AP entry. */
        char old_cidr[32];
        compute_ap_cidr_from_nvs(old_cidr, sizeof old_cidr);

        save_str_if_present(ap, "ssid",     "ap_ssid");
        save_str_if_present(ap, "password", "ap_passwd");
        save_int_if_present(ap, "channel",  "ap_channel");
        save_str_if_present(ap, "ip",       "ap_ip");
        save_str_if_present(ap, "mask",     "ap_mask");
        save_str_if_present(ap, "dns",      "ap_dns");
        const cJSON *hidden_j = cJSON_GetObjectItem(ap, "hidden");
        if (cJSON_IsBool(hidden_j)) {
            nvs_save_u8("ap_hidden", cJSON_IsTrue(hidden_j) ? 1 : 0);
        }

        /* DNS relay — live apply (no restart needed; the forwarder
         * picks up enable/upstream right away, and we re-run
         * softap_set_dns_addr so the DHCP-advertised resolver flips
         * for new lease requests). */
        const cJSON *dr = cJSON_GetObjectItem(ap, "dns_relay");
        bool dns_relay_touched = false;
        if (cJSON_IsObject(dr)) {
            const cJSON *en = cJSON_GetObjectItem(dr, "enabled");
            if (cJSON_IsBool(en)) {
                dns_relay_set_enabled(cJSON_IsTrue(en));
                dns_relay_touched = true;
            }
            const cJSON *up = cJSON_GetObjectItem(dr, "upstream");
            if (cJSON_IsString(up)) {
                uint32_t nbo = 0;
                if (up->valuestring[0]) {
                    ip4_addr_t a;
                    if (ip4addr_aton(up->valuestring, &a)) nbo = a.addr;
                }
                dns_relay_set_upstream(nbo);
                dns_relay_touched = true;
            }
        }
        /* No direct softap_set_dns_addr() here — the dns_relay task
         * fires dns_relay_on_healthy / dns_relay_on_unhealthy on every
         * health transition, and main.c's overrides re-run softap from
         * there. Calling it both places would cause a second DHCP-server
         * restart 12 s after the first and knock newly-leased clients
         * off their fresh DNS-server entry. */
        (void)dns_relay_touched;

        char new_cidr[32];
        char proposed[512];
        proposed[0] = '\0';
        compute_ap_cidr_from_nvs(new_cidr, sizeof new_cidr);
        bool offer = false;
        maintain_ap_cidr_in_routes(old_cidr, new_cidr, &offer,
                                   proposed, sizeof proposed);
        if (offer) {
            /* Stash proposal for the response — the SPA confirms with
             * the operator and POSTs proposed_routes back to
             * /api/tailscale. */
            cJSON_AddStringToObject(root, "_ap_cidr_offer", new_cidr);
            cJSON_AddStringToObject(root, "_proposed_routes", proposed);
        }
    }

    cJSON *resp_json = cJSON_CreateObject();
    /* nvs_save_errors_attach decides ok=true/false based on whether
     * any nvs_save_* call recorded a write failure for this request. */
    nvs_save_errors_attach(resp_json);
    cJSON_AddBoolToObject(resp_json, "restart_required", true);
    cJSON *offer = cJSON_GetObjectItem(root, "_ap_cidr_offer");
    cJSON *proposed = cJSON_GetObjectItem(root, "_proposed_routes");
    if (offer && cJSON_IsString(offer)) {
        cJSON_AddStringToObject(resp_json, "ap_cidr_offer", offer->valuestring);
        if (proposed && cJSON_IsString(proposed)) {
            cJSON_AddStringToObject(resp_json, "proposed_routes", proposed->valuestring);
        }
    }
    char *resp_str = cJSON_PrintUnformatted(resp_json);
    cJSON_Delete(resp_json);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, resp_str ? resp_str : "{\"ok\":true}");
    free(resp_str);
    return err;
}

static const httpd_uri_t uri_network_save = {
    .uri      = "/api/network",
    .method   = HTTP_POST,
    .handler  = network_save_handler,
    .user_ctx = NULL,
};

static const char *wifi_authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
        case WIFI_AUTH_ENTERPRISE:      return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK:        return "wapi";
        default:                        return "unknown";
    }
}

/* === Async WiFi scan ====================================================
 *
 * The old handler called esp_wifi_scan_start(blocking=true), which held
 * the single httpd-server task in a wait state for ~2 s per call. Any
 * other HTTP request landing in that window queued up and frequently
 * timed out client-side. The SPA-load alone fires 5–7 parallel fetches.
 *
 * Now: the GET /api/network/scan handler just KICKS a scan
 * (non-blocking) and returns immediately. The actual scan result lands
 * in s_scan_cache via the WIFI_EVENT_SCAN_DONE event handler. SPA
 * polls /api/network/scan/result every 500 ms until status == "ready".
 *
 * Handler latency goes from ~2 s to <10 ms; concurrent requests are
 * no longer serialised behind it. */

typedef enum {
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_READY,
    SCAN_ERROR,
} scan_state_t;

#define SCAN_CACHE_MAX 32
static volatile scan_state_t s_scan_state    = SCAN_IDLE;
static uint16_t              s_scan_count    = 0;
static wifi_ap_record_t      s_scan_cache[SCAN_CACHE_MAX];
static uint32_t              s_scan_done_ms  = 0;
static SemaphoreHandle_t     s_scan_mutex    = NULL;

static void scan_done_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT || id != WIFI_EVENT_SCAN_DONE) return;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > SCAN_CACHE_MAX) n = SCAN_CACHE_MAX;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (n) esp_wifi_scan_get_ap_records(&n, s_scan_cache);
    s_scan_count   = n;
    s_scan_done_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_scan_state   = SCAN_READY;
    xSemaphoreGive(s_scan_mutex);
    ESP_LOGI("web_ui", "async scan done: %u networks", (unsigned)n);
}

/* Lazy init — first scan request creates the mutex + subscribes to the
 * SCAN_DONE event. Keeps the init footprint zero until somebody actually
 * uses the scan endpoint. */
static void scan_init_once(void)
{
    if (s_scan_mutex) return;
    s_scan_mutex = xSemaphoreCreateMutex();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                scan_done_event, NULL);
}

static esp_err_t network_scan_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    scan_init_once();

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    bool kick = (s_scan_state != SCAN_RUNNING);
    if (kick) s_scan_state = SCAN_RUNNING;
    xSemaphoreGive(s_scan_mutex);

    if (kick) {
        /* Passive scan — listens for beacons, never disassociates the
         * STA (which used to kill the very TCP socket carrying this
         * request back in the synchronous era). */
        wifi_scan_config_t cfg = {
            .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_PASSIVE,
            .scan_time = { .passive = 120 },
        };
        if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
            xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
            s_scan_state = SCAN_ERROR;
            xSemaphoreGive(s_scan_mutex);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req,
                "{\"status\":\"error\",\"reason\":\"esp_wifi_scan_start failed\"}");
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
}

static esp_err_t network_scan_result_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    scan_init_once();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    scan_state_t st = s_scan_state;
    uint16_t n = (st == SCAN_READY) ? s_scan_count : 0;
    uint32_t age = (st == SCAN_READY)
        ? ((uint32_t)(esp_timer_get_time() / 1000) - s_scan_done_ms)
        : 0;
    for (uint16_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid",    (const char *)s_scan_cache[i].ssid);
        cJSON_AddNumberToObject(e, "rssi",    s_scan_cache[i].rssi);
        cJSON_AddNumberToObject(e, "channel", s_scan_cache[i].primary);
        cJSON_AddStringToObject(e, "auth",    wifi_authmode_str(s_scan_cache[i].authmode));
        cJSON_AddItemToArray(arr, e);
    }
    xSemaphoreGive(s_scan_mutex);

    const char *status_str = "idle";
    if (st == SCAN_RUNNING) status_str = "scanning";
    else if (st == SCAN_READY) status_str = "ready";
    else if (st == SCAN_ERROR) status_str = "error";
    cJSON_AddStringToObject(root, "status", status_str);
    cJSON_AddNumberToObject(root, "age_ms", age);
    cJSON_AddItemToObject(root, "networks", arr);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_network_scan = {
    .uri      = "/api/network/scan",
    .method   = HTTP_GET,
    .handler  = network_scan_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_network_scan_result = {
    .uri      = "/api/network/scan/result",
    .method   = HTTP_GET,
    .handler  = network_scan_result_handler,
    .user_ctx = NULL,
};

static esp_err_t tools_route_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    /* Parse ?dst=<ipv4>[&src=<ipv4>]. */
    char query[128];
    char dst_str[32];
    char src_str[32] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK
        || httpd_query_key_value(query, "dst", dst_str, sizeof dst_str) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dst");
        return ESP_FAIL;
    }
    /* src is optional — empty means "self-origin" (matches default behaviour). */
    (void)httpd_query_key_value(query, "src", src_str, sizeof src_str);

    ip4_addr_t a;
    if (!ip4addr_aton(dst_str, &a)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid IPv4");
        return ESP_FAIL;
    }
    /* route_explain takes host byte order. */
    uint32_t dst_hbo = lwip_ntohl(a.addr);
    uint32_t src_hbo = 0;
    if (src_str[0]) {
        ip4_addr_t sa;
        if (!ip4addr_aton(src_str, &sa)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid src IPv4");
            return ESP_FAIL;
        }
        src_hbo = lwip_ntohl(sa.addr);
    }

    char netif_name[16] = {0};
    char reason[160]    = {0};
    route_explain(src_hbo, dst_hbo, netif_name, sizeof netif_name, reason, sizeof reason);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dst",    dst_str);
    if (src_str[0]) cJSON_AddStringToObject(root, "src", src_str);
    cJSON_AddStringToObject(root, "netif",  netif_name);
    cJSON_AddStringToObject(root, "reason", reason);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_tools_route = {
    .uri      = "/api/tools/route",
    .method   = HTTP_GET,
    .handler  = tools_route_handler,
    .user_ctx = NULL,
};

/* Helper: read a single query param into a stack buffer. Returns ESP_OK
 * on success or NOT_FOUND if the key is missing. */
static esp_err_t tools_query_get(httpd_req_t *req, const char *key,
                                 char *out, size_t out_size)
{
    char qs[160];
    if (httpd_req_get_url_query_str(req, qs, sizeof qs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_query_key_value(qs, key, out, out_size);
}

static esp_err_t tools_ping_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char target[80];
    if (tools_query_get(req, "target", target, sizeof target) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing target");
        return ESP_FAIL;
    }
    char count_str[8];
    int count = 4;
    if (tools_query_get(req, "count", count_str, sizeof count_str) == ESP_OK) {
        count = atoi(count_str);
        if (count < 1)  count = 1;
        if (count > 10) count = 10;
    }

    /* net_diag writes plain text into the buffer; we expose it as text/plain
     * so the SPA can render it in a <pre>. */
    size_t buf_size = 2048;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[0] = '\0';

    net_diag_ping(target, count, 1000, buf, buf_size);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static esp_err_t tools_trace_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char target[80];
    if (tools_query_get(req, "target", target, sizeof target) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing target");
        return ESP_FAIL;
    }
    char hops_str[8];
    int max_hops = 16;
    if (tools_query_get(req, "max_hops", hops_str, sizeof hops_str) == ESP_OK) {
        max_hops = atoi(hops_str);
        if (max_hops < 1)  max_hops = 1;
        if (max_hops > 30) max_hops = 30;
    }

    size_t buf_size = 2048;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    buf[0] = '\0';

    net_diag_trace(target, max_hops, 1500, buf, buf_size);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static const httpd_uri_t uri_tools_ping = {
    .uri = "/api/tools/ping",  .method = HTTP_GET, .handler = tools_ping_handler,
};
static const httpd_uri_t uri_tools_trace = {
    .uri = "/api/tools/trace", .method = HTTP_GET, .handler = tools_trace_handler,
};

static esp_err_t tools_pcap_status_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode",            pcap_mode_to_string(pcap_get_mode()));
    cJSON_AddNumberToObject(root, "mode_value",      pcap_get_mode());
    cJSON_AddBoolToObject  (root, "client_connected", pcap_client_connected());
    cJSON_AddNumberToObject(root, "captured",        pcap_get_captured_count());
    cJSON_AddNumberToObject(root, "dropped",         pcap_get_dropped_count());
    cJSON_AddNumberToObject(root, "snaplen",         pcap_get_snaplen());
    size_t used = 0, total = 0;
    pcap_get_buffer_usage(&used, &total);
    cJSON_AddNumberToObject(root, "buf_used",  used);
    cJSON_AddNumberToObject(root, "buf_total", total);
    cJSON_AddNumberToObject(root, "tcp_port",  19000);   /* hardcoded in pcap_capture.c */

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t tools_pcap_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char buf[128];
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    const cJSON *m = cJSON_GetObjectItem(body, "mode");
    if (cJSON_IsString(m)) {
        if      (!strcmp(m->valuestring, "off"))         pcap_set_mode(PCAP_MODE_OFF);
        else if (!strcmp(m->valuestring, "acl_monitor")) pcap_set_mode(PCAP_MODE_ACL_MONITOR);
        else if (!strcmp(m->valuestring, "promiscuous")) pcap_set_mode(PCAP_MODE_PROMISCUOUS);
    }
    const cJSON *s = cJSON_GetObjectItem(body, "snaplen");
    if (cJSON_IsNumber(s)) pcap_set_snaplen((uint16_t)s->valuedouble);
    cJSON_Delete(body);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_tools_pcap_get = {
    .uri = "/api/tools/pcap", .method = HTTP_GET,  .handler = tools_pcap_status_handler,
};
static const httpd_uri_t uri_tools_pcap_set = {
    .uri = "/api/tools/pcap", .method = HTTP_POST, .handler = tools_pcap_save_handler,
};

static esp_err_t firewall_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root  = cJSON_CreateObject();
    cJSON *lists = cJSON_CreateArray();

    acl_lock();
    for (int i = 0; i < MAX_ACL_LISTS; i++) {
        cJSON *list = cJSON_CreateObject();
        cJSON_AddNumberToObject(list, "index", i);
        cJSON_AddStringToObject(list, "name",  acl_get_name(i));
        cJSON_AddStringToObject(list, "desc",  acl_get_desc(i));

        acl_stats_t *st = acl_get_stats(i);
        cJSON *stats = cJSON_CreateObject();
        cJSON_AddNumberToObject(stats, "allowed", st ? st->packets_allowed : 0);
        cJSON_AddNumberToObject(stats, "denied",  st ? st->packets_denied  : 0);
        cJSON_AddNumberToObject(stats, "nomatch", st ? st->packets_nomatch : 0);
        cJSON_AddItemToObject(list, "stats", stats);

        cJSON *rules = cJSON_CreateArray();
        acl_entry_t *entries = acl_get_rules(i);
        if (entries) {
            for (int j = 0; j < MAX_ACL_ENTRIES; j++) {
                if (!entries[j].valid) break;   /* list is compacted */

                cJSON *r = cJSON_CreateObject();
                cJSON_AddNumberToObject(r, "index", j);
                char buf[28];
                acl_format_ip(entries[j].src,  entries[j].s_mask, buf, sizeof buf);
                cJSON_AddStringToObject(r, "src",  buf);
                acl_format_ip(entries[j].dest, entries[j].d_mask, buf, sizeof buf);
                cJSON_AddStringToObject(r, "dest", buf);
                cJSON_AddNumberToObject(r, "proto",  entries[j].proto);
                cJSON_AddNumberToObject(r, "s_port", entries[j].s_port);
                cJSON_AddNumberToObject(r, "d_port", entries[j].d_port);
                cJSON_AddNumberToObject(r, "action", entries[j].allow & 0x01);
                cJSON_AddBoolToObject  (r, "monitor", (entries[j].allow & ACL_MONITOR) != 0);
                cJSON_AddNumberToObject(r, "hits",   entries[j].hit_count);
                cJSON_AddItemToArray(rules, r);
            }
        }
        cJSON_AddItemToObject(list, "rules", rules);
        cJSON_AddItemToArray(lists, list);
    }
    acl_unlock();

    cJSON_AddItemToObject(root, "lists", lists);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_firewall = {
    .uri      = "/api/firewall",
    .method   = HTTP_GET,
    .handler  = firewall_handler,
    .user_ctx = NULL,
};

/* Helper: pull a uint8 ACL list index out of the JSON body. */
static int parse_acl_index(const cJSON *body)
{
    const cJSON *idx = body ? cJSON_GetObjectItem(body, "acl") : NULL;
    if (!cJSON_IsNumber(idx)) return -1;
    int n = (int)idx->valuedouble;
    if (n < 0 || n >= MAX_ACL_LISTS) return -1;
    return n;
}

static esp_err_t firewall_add_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char body_buf[512];
    if (recv_body(req, body_buf, sizeof body_buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *body = cJSON_Parse(body_buf);
    int acl_no = parse_acl_index(body);
    const cJSON *src_j   = body ? cJSON_GetObjectItem(body, "src")   : NULL;
    const cJSON *dest_j  = body ? cJSON_GetObjectItem(body, "dest")  : NULL;
    if (acl_no < 0 || !cJSON_IsString(src_j) || !cJSON_IsString(dest_j)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing acl/src/dest");
        return ESP_FAIL;
    }

    uint32_t src, s_mask, dest, d_mask;
    if (!acl_parse_ip(src_j->valuestring,  &src,  &s_mask) ||
        !acl_parse_ip(dest_j->valuestring, &dest, &d_mask)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad src/dest CIDR");
        return ESP_FAIL;
    }

    /* Optional fields default to "any". */
    const cJSON *pr = cJSON_GetObjectItem(body, "proto");
    const cJSON *sp = cJSON_GetObjectItem(body, "s_port");
    const cJSON *dp = cJSON_GetObjectItem(body, "d_port");
    const cJSON *ac = cJSON_GetObjectItem(body, "action");
    const cJSON *mn = cJSON_GetObjectItem(body, "monitor");

    uint8_t  proto  = cJSON_IsNumber(pr) ? (uint8_t) pr->valuedouble : 0;
    uint16_t s_port = cJSON_IsNumber(sp) ? (uint16_t)sp->valuedouble : 0;
    uint16_t d_port = cJSON_IsNumber(dp) ? (uint16_t)dp->valuedouble : 0;
    uint8_t  allow  = cJSON_IsNumber(ac) ? (uint8_t) ac->valuedouble : ACL_ALLOW;
    if (cJSON_IsTrue(mn)) allow |= ACL_MONITOR;

    bool ok = acl_add((uint8_t)acl_no, src, s_mask, dest, d_mask,
                      proto, s_port, d_port, allow);
    cJSON_Delete(body);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "list full");
        return ESP_FAIL;
    }
    nvs_save_errors_reset();
    esp_err_t serr = save_acl_rules();
    if (serr != ESP_OK) nvs_save_record_err("acl", serr);
    return send_save_response(req);
}

static esp_err_t firewall_delete_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char body_buf[128];
    if (recv_body(req, body_buf, sizeof body_buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *body = cJSON_Parse(body_buf);
    int acl_no = parse_acl_index(body);
    const cJSON *rule_j = body ? cJSON_GetObjectItem(body, "index") : NULL;
    if (acl_no < 0 || !cJSON_IsNumber(rule_j)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing acl/index");
        return ESP_FAIL;
    }
    int rule_idx = (int)rule_j->valuedouble;
    cJSON_Delete(body);

    if (rule_idx < 0 || rule_idx >= MAX_ACL_ENTRIES
        || !acl_delete((uint8_t)acl_no, (uint8_t)rule_idx)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    nvs_save_errors_reset();
    esp_err_t serr = save_acl_rules();
    if (serr != ESP_OK) nvs_save_record_err("acl", serr);
    return send_save_response(req);
}

static esp_err_t firewall_clear_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char body_buf[128];
    if (recv_body(req, body_buf, sizeof body_buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *body = cJSON_Parse(body_buf);
    int acl_no = parse_acl_index(body);
    cJSON_Delete(body);
    if (acl_no < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing acl");
        return ESP_FAIL;
    }

    acl_clear((uint8_t)acl_no);
    nvs_save_errors_reset();
    esp_err_t serr = save_acl_rules();
    if (serr != ESP_OK) nvs_save_record_err("acl", serr);
    return send_save_response(req);
}

static const httpd_uri_t uri_firewall_add = {
    .uri = "/api/firewall/add",    .method = HTTP_POST, .handler = firewall_add_handler,
};
static const httpd_uri_t uri_firewall_delete = {
    .uri = "/api/firewall/delete", .method = HTTP_POST, .handler = firewall_delete_handler,
};
static const httpd_uri_t uri_firewall_clear = {
    .uri = "/api/firewall/clear",  .method = HTTP_POST, .handler = firewall_clear_handler,
};

/* ───────────────────────── DHCP reservations ────────────────────────
 * GET  /api/dhcp/reservations  → { reservations: [{mac,ip,name}...], max }
 * POST /api/dhcp/reservations  → body { reservations: [...] } replaces the
 *                                whole table. Empty/invalid entries are
 *                                silently dropped. */

static bool parse_mac_str(const char *s, uint8_t out[6])
{
    if (!s) return false;
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6
        && sscanf(s, "%x-%x-%x-%x-%x-%x",
                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static esp_err_t dhcp_reservations_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    for (int i = 0; i < DHCP_RESERVATIONS_MAX; i++) {
        dhcp_reservation_t r;
        if (!dhcp_reservations_get(i, &r)) continue;

        char mac_str[18];
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);

        ip4_addr_t a = { .addr = r.ip };
        char ip_str[16];
        snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&a));

        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "mac",  mac_str);
        cJSON_AddStringToObject(e, "ip",   ip_str);
        cJSON_AddStringToObject(e, "name", r.name);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(root, "reservations", arr);
    cJSON_AddNumberToObject(root, "max", DHCP_RESERVATIONS_MAX);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t dhcp_reservations_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    /* Heap buffer — the table can grow up to 16 entries and cJSON
     * parsing piles on top of the httpd worker stack. */
    size_t buf_size = 4096;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "reservations");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing reservations[]");
        return ESP_FAIL;
    }

    dhcp_reservation_t out[DHCP_RESERVATIONS_MAX];
    memset(out, 0, sizeof out);
    int n_in  = cJSON_GetArraySize(arr);
    int n_out = 0;

    for (int i = 0; i < n_in && n_out < DHCP_RESERVATIONS_MAX; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(e)) continue;

        cJSON *mac_j = cJSON_GetObjectItem(e, "mac");
        cJSON *ip_j  = cJSON_GetObjectItem(e, "ip");
        if (!cJSON_IsString(mac_j) || !cJSON_IsString(ip_j)) continue;

        dhcp_reservation_t *r = &out[n_out];
        if (!parse_mac_str(mac_j->valuestring, r->mac)) continue;

        ip4_addr_t a;
        if (!ip4addr_aton(ip_j->valuestring, &a) || a.addr == 0) continue;
        r->ip = a.addr;

        cJSON *name_j = cJSON_GetObjectItem(e, "name");
        if (cJSON_IsString(name_j)) {
            strlcpy(r->name, name_j->valuestring, sizeof r->name);
        }
        r->valid = 1;
        n_out++;
    }
    cJSON_Delete(root);

    esp_err_t err = dhcp_reservations_set_all(out, n_out);

    /* Reservations apply on the next DHCP REQUEST — the table is hot-
     * reloaded into the lookup cache, so no reboot is required. Clients
     * already holding a non-matching lease keep it until expiry. */
    nvs_save_errors_reset();
    if (err != ESP_OK) nvs_save_record_err("dhcp_res", err);
    return send_save_response(req);
}

static const httpd_uri_t uri_dhcp_reservations = {
    .uri      = "/api/dhcp/reservations",
    .method   = HTTP_GET,
    .handler  = dhcp_reservations_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t uri_dhcp_reservations_save = {
    .uri      = "/api/dhcp/reservations",
    .method   = HTTP_POST,
    .handler  = dhcp_reservations_save_handler,
    .user_ctx = NULL,
};

/* GET /api/dhcp/leases — { clients:[{mac,ip,hostname,name,rssi,reserved}...],
 *                          leases :[{mac,ip,hostname,lease_remaining}...] }
 *
 * `clients` is the current AP station list, joined with the active-lease
 * table for IP + hostname, then overlaid with the reservation name when
 * one exists for that MAC.
 *
 * `leases` is the raw active-lease snapshot — a station that has fallen
 * off the air still appears here until its lease expires. */

#define DHCP_LEASES_MAX_REPORT 16

static esp_err_t dhcp_leases_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    /* Pull both views first so we can cross-reference them in one pass. */
    dhcp_lease_info_t leases[DHCP_LEASES_MAX_REPORT];
    int lease_count = dhcps_get_active_leases(leases, DHCP_LEASES_MAX_REPORT);

    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof sta_list);
    esp_wifi_ap_get_sta_list(&sta_list);

    cJSON *root         = cJSON_CreateObject();
    cJSON *clients_arr  = cJSON_CreateArray();
    cJSON *leases_arr   = cJSON_CreateArray();
    if (!root || !clients_arr || !leases_arr) {
        cJSON_Delete(root); cJSON_Delete(clients_arr); cJSON_Delete(leases_arr);
        httpd_resp_send_500(req); return ESP_FAIL;
    }

    /* Connected stations — for each, find its lease and reservation. */
    for (int i = 0; i < sta_list.num; i++) {
        wifi_sta_info_t *sta = &sta_list.sta[i];

        char mac_str[18];
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 sta->mac[0], sta->mac[1], sta->mac[2],
                 sta->mac[3], sta->mac[4], sta->mac[5]);

        const char *hostname = "";
        uint32_t    ip_nbo   = 0;
        for (int j = 0; j < lease_count; j++) {
            if (memcmp(leases[j].mac, sta->mac, 6) == 0) {
                hostname = leases[j].hostname;
                ip_nbo   = leases[j].ip;
                break;
            }
        }

        const char *res_name = dhcp_reservations_lookup_name_by_mac(sta->mac);
        bool reserved        = dhcp_reservations_lookup(sta->mac) != 0;

        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "mac", mac_str);
        if (ip_nbo) {
            ip4_addr_t a = { .addr = ip_nbo };
            char ip_str[16];
            snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&a));
            cJSON_AddStringToObject(e, "ip", ip_str);
        } else {
            cJSON_AddStringToObject(e, "ip", "");
        }
        cJSON_AddStringToObject(e, "hostname", hostname);
        cJSON_AddStringToObject(e, "name",     res_name ? res_name : "");
        cJSON_AddNumberToObject(e, "rssi",     sta->rssi);
        cJSON_AddBoolToObject  (e, "reserved", reserved);
        cJSON_AddItemToArray(clients_arr, e);
    }

    /* Raw lease snapshot. */
    for (int j = 0; j < lease_count; j++) {
        char mac_str[18];
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 leases[j].mac[0], leases[j].mac[1], leases[j].mac[2],
                 leases[j].mac[3], leases[j].mac[4], leases[j].mac[5]);

        ip4_addr_t a = { .addr = leases[j].ip };
        char ip_str[16];
        snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&a));

        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "mac",      mac_str);
        cJSON_AddStringToObject(e, "ip",       ip_str);
        cJSON_AddStringToObject(e, "hostname", leases[j].hostname);
        cJSON_AddNumberToObject(e, "lease_remaining", leases[j].lease_timer);
        cJSON_AddItemToArray(leases_arr, e);
    }

    cJSON_AddItemToObject(root, "clients", clients_arr);
    cJSON_AddItemToObject(root, "leases",  leases_arr);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_dhcp_leases = {
    .uri      = "/api/dhcp/leases",
    .method   = HTTP_GET,
    .handler  = dhcp_leases_handler,
    .user_ctx = NULL,
};

/* POST /api/dhcp/kick — body { "mac": "aa:bb:cc:dd:ee:ff" } — deauths
 * the station so it must re-associate, which also triggers a fresh DHCP
 * exchange. Useful right after changing a reservation: clients normally
 * hold their current lease until expiry. */

static esp_err_t dhcp_kick_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char body_buf[128];
    if (recv_body(req, body_buf, sizeof body_buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body_buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *mac_j = cJSON_GetObjectItem(root, "mac");
    if (!cJSON_IsString(mac_j)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mac");
        return ESP_FAIL;
    }

    uint8_t target_mac[6];
    if (!parse_mac_str(mac_j->valuestring, target_mac)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    /* Look up the AID by walking the station list — esp_wifi_deauth_sta
     * wants an AID rather than a MAC. AID 0 means "every station". */
    wifi_sta_list_t sta_list;
    memset(&sta_list, 0, sizeof sta_list);
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int matched_aid = -1;
    for (int i = 0; i < sta_list.num; i++) {
        if (memcmp(sta_list.sta[i].mac, target_mac, 6) == 0) {
            /* AID indices in this struct are 1-based from the order the
             * station joined. The wifi driver exposes them via
             * esp_wifi_ap_get_sta_aid; falling back on the array index
             * works for the common case but the API is the canonical
             * source. */
            uint16_t aid = 0;
            if (esp_wifi_ap_get_sta_aid(target_mac, &aid) == ESP_OK && aid > 0) {
                matched_aid = aid;
            }
            break;
        }
    }

    httpd_resp_set_type(req, "application/json");
    if (matched_aid < 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"reason\":\"not connected\"}");
    }

    esp_err_t err = esp_wifi_deauth_sta((uint16_t)matched_aid);
    if (err != ESP_OK) {
        char resp[96];
        snprintf(resp, sizeof resp, "{\"ok\":false,\"reason\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, resp);
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_dhcp_kick = {
    .uri      = "/api/dhcp/kick",
    .method   = HTTP_POST,
    .handler  = dhcp_kick_handler,
    .user_ctx = NULL,
};

/* ───────────────────────── Port forwarding ─────────────────────────
 * GET  /api/portmap  → { mappings:[{proto,ext_port,int_ip,int_port,name}...], max }
 * POST /api/portmap  → same shape, replaces the whole table. */

static const char *portmap_proto_str(uint8_t p)
{
    if (p == PORTMAP_PROTO_TCP) return "tcp";
    if (p == PORTMAP_PROTO_UDP) return "udp";
    return "?";
}

static uint8_t portmap_proto_from_str(const char *s)
{
    if (!s) return 0;
    if (!strcasecmp(s, "tcp")) return PORTMAP_PROTO_TCP;
    if (!strcasecmp(s, "udp")) return PORTMAP_PROTO_UDP;
    return 0;
}

static esp_err_t portmap_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root); cJSON_Delete(arr);
        httpd_resp_send_500(req); return ESP_FAIL;
    }

    for (int i = 0; i < PORTMAP_MAX; i++) {
        portmap_entry_t e;
        if (!portmap_get(i, &e)) continue;

        ip4_addr_t a = { .addr = e.int_ip };
        char ip_str[16];
        snprintf(ip_str, sizeof ip_str, IPSTR, IP2STR(&a));

        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "proto",    portmap_proto_str(e.proto));
        cJSON_AddNumberToObject(j, "ext_port", e.ext_port);
        cJSON_AddStringToObject(j, "int_ip",   ip_str);
        cJSON_AddNumberToObject(j, "int_port", e.int_port);
        cJSON_AddStringToObject(j, "name",     e.name);
        cJSON_AddItemToArray(arr, j);
    }
    cJSON_AddItemToObject(root, "mappings", arr);
    cJSON_AddNumberToObject(root, "max", PORTMAP_MAX);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t portmap_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    size_t buf_size = 4096;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "mappings");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mappings[]");
        return ESP_FAIL;
    }

    portmap_entry_t out[PORTMAP_MAX];
    memset(out, 0, sizeof out);
    int n_in  = cJSON_GetArraySize(arr);
    int n_out = 0;

    for (int i = 0; i < n_in && n_out < PORTMAP_MAX; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(e)) continue;

        const cJSON *proto_j = cJSON_GetObjectItem(e, "proto");
        const cJSON *ep_j    = cJSON_GetObjectItem(e, "ext_port");
        const cJSON *iip_j   = cJSON_GetObjectItem(e, "int_ip");
        const cJSON *ip_j    = cJSON_GetObjectItem(e, "int_port");
        const cJSON *name_j  = cJSON_GetObjectItem(e, "name");

        if (!cJSON_IsString(proto_j) || !cJSON_IsNumber(ep_j)
            || !cJSON_IsString(iip_j) || !cJSON_IsNumber(ip_j)) continue;

        portmap_entry_t *r = &out[n_out];
        r->proto = portmap_proto_from_str(proto_j->valuestring);
        if (!r->proto) continue;

        int ep = (int)ep_j->valuedouble;
        int ip = (int)ip_j->valuedouble;
        if (ep <= 0 || ep > 65535 || ip <= 0 || ip > 65535) continue;
        r->ext_port = (uint16_t)ep;
        r->int_port = (uint16_t)ip;

        ip4_addr_t a;
        if (!ip4addr_aton(iip_j->valuestring, &a) || a.addr == 0) continue;
        r->int_ip = a.addr;

        if (cJSON_IsString(name_j)) {
            strlcpy(r->name, name_j->valuestring, sizeof r->name);
        }
        r->valid = 1;
        n_out++;
    }
    cJSON_Delete(root);

    esp_err_t err = portmap_set_all(out, n_out);

    nvs_save_errors_reset();
    if (err != ESP_OK) nvs_save_record_err("portmap", err);
    return send_save_response(req);
}

static const httpd_uri_t uri_portmap = {
    .uri      = "/api/portmap",
    .method   = HTTP_GET,
    .handler  = portmap_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t uri_portmap_save = {
    .uri      = "/api/portmap",
    .method   = HTTP_POST,
    .handler  = portmap_save_handler,
    .user_ctx = NULL,
};

/* ─────────────────── MAC denylist ─────────────────── */

static esp_err_t mac_deny_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root); cJSON_Delete(arr);
        httpd_resp_send_500(req); return ESP_FAIL;
    }

    for (int i = 0; i < MAC_DENY_MAX; i++) {
        mac_deny_entry_t e;
        if (!mac_deny_get(i, &e)) continue;

        char mac_str[18];
        snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                 e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);

        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "mac",  mac_str);
        cJSON_AddStringToObject(j, "name", e.name);
        cJSON_AddItemToArray(arr, j);
    }
    cJSON_AddItemToObject(root, "denylist", arr);
    cJSON_AddNumberToObject(root, "max", MAC_DENY_MAX);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t mac_deny_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    size_t buf_size = 2048;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "denylist");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing denylist[]");
        return ESP_FAIL;
    }

    mac_deny_entry_t out[MAC_DENY_MAX];
    memset(out, 0, sizeof out);
    int n_in  = cJSON_GetArraySize(arr);
    int n_out = 0;

    for (int i = 0; i < n_in && n_out < MAC_DENY_MAX; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(e)) continue;
        cJSON *mac_j  = cJSON_GetObjectItem(e, "mac");
        cJSON *name_j = cJSON_GetObjectItem(e, "name");
        if (!cJSON_IsString(mac_j)) continue;

        mac_deny_entry_t *r = &out[n_out];
        if (!parse_mac_str(mac_j->valuestring, r->mac)) continue;
        if (cJSON_IsString(name_j)) {
            strlcpy(r->name, name_j->valuestring, sizeof r->name);
        }
        r->valid = 1;
        n_out++;
    }
    cJSON_Delete(root);

    esp_err_t err = mac_deny_set_all(out, n_out);

    /* Kick any currently-connected station whose MAC is now denied —
     * otherwise the operator has to manually click Kick. */
    if (err == ESP_OK) {
        wifi_sta_list_t sl;
        memset(&sl, 0, sizeof sl);
        if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
            for (int i = 0; i < sl.num; i++) {
                if (mac_deny_is_blocked(sl.sta[i].mac)) {
                    uint16_t aid = 0;
                    if (esp_wifi_ap_get_sta_aid(sl.sta[i].mac, &aid) == ESP_OK
                        && aid > 0) {
                        esp_wifi_deauth_sta(aid);
                    }
                }
            }
        }
    }

    nvs_save_errors_reset();
    if (err != ESP_OK) nvs_save_record_err("mac_deny", err);
    return send_save_response(req);
}

static const httpd_uri_t uri_mac_deny = {
    .uri      = "/api/mac/denylist",
    .method   = HTTP_GET,
    .handler  = mac_deny_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t uri_mac_deny_save = {
    .uri      = "/api/mac/denylist",
    .method   = HTTP_POST,
    .handler  = mac_deny_save_handler,
    .user_ctx = NULL,
};

/* ─────────────────── Pre-crash log buffer ───────────────────
 * The RTC slow-RAM ring captures log lines all the way to the panic
 * and survives soft reset / watchdog / abort. Cleared on cold boot or
 * brown-out. UI lives in the Tools tab. */

static esp_err_t log_precrash_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    bool have = log_capture_have_precrash();
    size_t sz = log_capture_precrash_size();
    cJSON_AddBoolToObject  (root, "have", have);
    cJSON_AddNumberToObject(root, "size", (double)sz);

    if (have && sz > 0) {
        /* Cap to a reasonable web payload — same ceiling as the live
         * log_tail. Pre-crash buffer can't legally exceed the RTC ring
         * size anyway. */
        size_t cap = WEB_UI_LOG_SNAPSHOT_BYTES;
        char *buf = malloc(cap);
        if (buf) {
            size_t n = log_capture_read_precrash(buf, cap - 1);
            buf[n] = '\0';
            cJSON_AddStringToObject(root, "text", buf);
            free(buf);
        } else {
            cJSON_AddStringToObject(root, "text", "");
        }
    } else {
        cJSON_AddStringToObject(root, "text", "");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t log_precrash_clear_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    log_capture_clear_precrash();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_log_precrash = {
    .uri      = "/api/log/precrash",
    .method   = HTTP_GET,
    .handler  = log_precrash_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t uri_log_precrash_clear = {
    .uri      = "/api/log/precrash/clear",
    .method   = HTTP_POST,
    .handler  = log_precrash_clear_handler,
    .user_ctx = NULL,
};

/* GET /api/log/raw?since=<seq> — JSON delta endpoint for the live log
 * tail poller. since=0 (the default) returns the full preserved ring;
 * non-zero asks for "everything appended after I last saw seq N".
 * "lost":true means the ring wrapped past the caller's cursor, so the
 * client should clear its view and treat data as a fresh dump. */
static esp_err_t log_raw_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    uint64_t since = 0;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *q = malloc(qlen);
        if (q && httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
            char val[32] = {0};
            if (httpd_query_key_value(q, "since", val, sizeof val) == ESP_OK) {
                since = strtoull(val, NULL, 10);
            }
        }
        free(q);
    }

    /* Scratch matches the full ring so a single poll can return
     * everything written since the client's cursor — otherwise
     * log_capture_read_since truncates to the newest scratch_sz
     * bytes, and the handler's "lost" check (caller fell behind)
     * fires spuriously on every burst that exceeds the scratch. */
    size_t cap = log_capture_capacity();
    size_t scratch_sz = cap;
    char *scratch = malloc(scratch_sz + 1);
    if (!scratch) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    uint64_t new_seq = 0;
    size_t n_raw = log_capture_read_since(since, scratch, scratch_sz + 1, &new_seq);
    /* "lost" means the ring's oldest preserved byte is newer than
     * what the caller already saw, i.e. they fell behind the ring
     * wrap. The previous formulation (new_seq - n_raw > since)
     * fired any time the scratch buffer was smaller than the gap,
     * which the bigger scratch above now prevents — but the more
     * accurate definition is "is the caller's cursor BEFORE the
     * oldest byte we kept?" which is what we test here. */
    uint64_t oldest = (new_seq > (uint64_t)log_capture_size())
                    ? new_seq - (uint64_t)log_capture_size() : 0;
    bool lost = (since != 0) && (since < oldest);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char hdr[160];
    snprintf(hdr, sizeof hdr,
             "{\"seq\":%llu,\"lost\":%s,\"size\":%u,\"cap\":%u,\"data\":\"",
             (unsigned long long)new_seq, lost ? "true" : "false",
             (unsigned)log_capture_size(),
             (unsigned)log_capture_capacity());
    httpd_resp_sendstr_chunk(req, hdr);

    /* Stream JSON-escaped payload. Buffer into 1 KB chunks before
     * calling httpd_resp_send_chunk(): the previous one-byte-per-call
     * implementation generated one TCP PSH/ACK pair per character,
     * and the per-poll latency over a 10 KB log was ~15 s end-to-end —
     * which then stalled the SPA's other parallel fetches behind it.
     * Now: ~10 chunk-send calls per 10 KB instead of 10 000. */
    char  obuf[1024];
    size_t op = 0;
    #define LRH_FLUSH() do { \
        if (op > 0) { httpd_resp_send_chunk(req, obuf, op); op = 0; } \
    } while (0)
    #define LRH_EMIT(s, l) do { \
        if (op + (size_t)(l) > sizeof obuf) LRH_FLUSH(); \
        memcpy(obuf + op, (s), (l)); op += (l); \
    } while (0)
    for (size_t i = 0; i < n_raw; i++) {
        unsigned char c = (unsigned char)scratch[i];
        if      (c == '"')  LRH_EMIT("\\\"", 2);
        else if (c == '\\') LRH_EMIT("\\\\", 2);
        else if (c == '\n') LRH_EMIT("\\n",  2);
        else if (c == '\r') LRH_EMIT("\\r",  2);
        else if (c == '\t') LRH_EMIT("\\t",  2);
        else if (c < 0x20) {
            char esc[8];
            int el = snprintf(esc, sizeof esc, "\\u%04x", c);
            LRH_EMIT(esc, el);
        } else {
            LRH_EMIT((const char *)&c, 1);
        }
    }
    LRH_FLUSH();
    #undef LRH_EMIT
    #undef LRH_FLUSH
    free(scratch);
    httpd_resp_sendstr_chunk(req, "\"}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* POST /api/log/clear — wipes the live ring (precrash buffer is on its
 * own clear endpoint). */
static esp_err_t log_clear_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    log_capture_clear();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_log_raw = {
    .uri      = "/api/log/raw",
    .method   = HTTP_GET,
    .handler  = log_raw_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t uri_log_clear = {
    .uri      = "/api/log/clear",
    .method   = HTTP_POST,
    .handler  = log_clear_handler,
    .user_ctx = NULL,
};

/* Format a host-byte-order IPv4 as "a.b.c.d" — used for the accepted-
 * routes table where host order is the documented storage. */
static void ip4_hbo_to_str(uint32_t hbo, char *out, size_t out_size)
{
    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned)((hbo >> 24) & 0xff),
             (unsigned)((hbo >> 16) & 0xff),
             (unsigned)((hbo >> 8)  & 0xff),
             (unsigned)( hbo        & 0xff));
}

static esp_err_t tailscale_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Settings — auth_key itself is never serialised, but we DO emit
     * enough metadata (set flag + prefix/suffix fingerprint + length)
     * for the operator to tell *which* key is currently stored without
     * leaking the secret to anyone reading the response. */
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddBoolToObject  (settings, "enabled",                 tailscale_enabled != 0);
    {
        const char *ak = tailscale_auth_key ? tailscale_auth_key : "";
        size_t alen = strlen(ak);
        cJSON_AddBoolToObject(settings, "auth_key_set", alen > 0);
        if (alen > 0) {
            cJSON_AddNumberToObject(settings, "auth_key_len", (int)alen);
            char preview[40];
            if (alen <= 8) {
                /* tiny strings (operator typed e.g. "test") — show
                 * verbatim, leaking such a value isn't meaningful. */
                snprintf(preview, sizeof preview, "%s", ak);
            } else if (alen <= 19) {
                /* short — just prefix + …, no tail fingerprint room */
                snprintf(preview, sizeof preview, "%.11s…", ak);
            } else {
                /* normal real key — 11-char protocol prefix +
                 * 4 fingerprint chars + … + last 4 chars. 8 unique
                 * chars is enough to tell two keys apart, far too few
                 * to brute-force back into the original 70+ char key. */
                snprintf(preview, sizeof preview, "%.15s…%s",
                         ak, ak + alen - 4);
            }
            cJSON_AddStringToObject(settings, "auth_key_preview", preview);
        }
    }
    if (tailscale_hostname)         cJSON_AddStringToObject(settings, "hostname",       tailscale_hostname);
    if (tailscale_login_server)     cJSON_AddStringToObject(settings, "login_server",   tailscale_login_server);
    if (tailscale_advertise_routes) cJSON_AddStringToObject(settings, "advertise_routes", tailscale_advertise_routes);
    cJSON_AddNumberToObject(settings, "max_peers",               tailscale_max_peers);
    cJSON_AddNumberToObject(settings, "default_derp_region",     tailscale_default_derp_region);
    cJSON_AddBoolToObject  (settings, "netcheck_override",       tailscale_netcheck_override != 0);
    cJSON_AddNumberToObject(settings, "netcheck_threshold_ms",   tailscale_netcheck_threshold_ms);
    cJSON_AddBoolToObject  (settings, "lan_bypass",              tailscale_lan_bypass != 0);
    cJSON_AddBoolToObject  (settings, "accept_routes",           tailscale_accept_routes != 0);
    if (tailscale_exit_node_ip) {
        /* tailscale_exit_node_ip is documented as host byte order. */
        char buf[16];
        ip4_hbo_to_str(tailscale_exit_node_ip, buf, sizeof buf);
        cJSON_AddStringToObject(settings, "exit_node_ip", buf);
    }
    cJSON_AddItemToObject(root, "settings", settings);

    /* Runtime state. tailscale_is_connected() refreshes tunnel_ip from
     * microlink, so the SPA gets a live tunnel address on every fetch. */
    bool ts_runtime_connected = tailscale_is_connected();
    cJSON *runtime = cJSON_CreateObject();
    cJSON_AddBoolToObject(runtime, "connected", ts_runtime_connected);
    if (tailscale_tunnel_ip) {
        char buf[16];
        ip4_to_str(tailscale_tunnel_ip, buf, sizeof buf);
        cJSON_AddStringToObject(runtime, "tunnel_ip", buf);
    }
    /* Last RegisterResponse User block — surfaces auth_key failures
     * that Headscale wraps in a 200-OK (User.ID=0 + empty name → auth
     * invalid or stale node-key). microlink_get_diag is the cheap path:
     * it just copies cached fields, no extra round-trips. */
    {
        struct microlink_s *mlh = tailscale_get_microlink();
        if (mlh) {
            microlink_diag_t diag;
            if (microlink_get_diag(mlh, &diag) == ESP_OK) {
                cJSON_AddNumberToObject(runtime, "register_user_id",
                                        diag.register_user_id);
                if (diag.register_user_name[0]) {
                    cJSON_AddStringToObject(runtime, "register_user_name",
                                            diag.register_user_name);
                }
                cJSON_AddBoolToObject  (runtime, "identity_persistent",
                                        diag.identity_persistent);
                cJSON_AddStringToObject(runtime, "identity_pubkey_prefix",
                                        diag.identity_pubkey_prefix);
            }
        }
    }
    cJSON_AddItemToObject(root, "runtime", runtime);

    /* MTU / MSS / PMTU manager — persisted fields (mode, fixed_mtu)
     * plus the live-derived effective values the AP-side netif hooks
     * actually enforce on every TCP SYN + DF=1 packet. source tells
     * the operator why the eff_mtu is what it is ("auto-direct" /
     * "auto-DERP" / "user" / "off"). */
    {
        ts_mtu_state_t mst = tailscale_mtu_get();
        cJSON *mtu = cJSON_CreateObject();
        cJSON_AddNumberToObject(mtu, "mode",      (int)mst.mode);
        cJSON_AddNumberToObject(mtu, "fixed_mtu", mst.fixed_mtu);
        cJSON_AddNumberToObject(mtu, "eff_mtu",   mst.eff_mtu);
        cJSON_AddNumberToObject(mtu, "eff_mss",   mst.eff_mss);
        cJSON_AddNumberToObject(mtu, "eff_pmtu",  mst.eff_pmtu);
        cJSON_AddStringToObject(mtu, "source",    mst.source ? mst.source : "");
        cJSON_AddItemToObject(root, "mtu", mtu);
    }

    /* Peers — empty array when microlink isn't running. While the tunnel
     * is still Registering, microlink_peer_info_t.online + .direct_path
     * are unreliable: the control plane has handed us the peer list but
     * DISCO hasn't run, so everyone briefly looks online + DERP. Mask
     * both fields to false until we're verifiably Connected — better a
     * grey dot for a few seconds than a misleading green one. */
    cJSON *peers = cJSON_CreateArray();
    struct microlink_s *ml = tailscale_get_microlink();
    if (ml) {
        int n = microlink_get_peer_count(ml);
        for (int i = 0; i < n; i++) {
            microlink_peer_info_t pi;
            if (microlink_get_peer_info(ml, i, &pi) != ESP_OK) continue;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "hostname",     pi.hostname);
            cJSON_AddBoolToObject  (p, "online",       ts_runtime_connected && pi.online);
            cJSON_AddBoolToObject  (p, "direct_path",  ts_runtime_connected && pi.direct_path);
            cJSON_AddBoolToObject  (p, "is_exit_node", pi.is_exit_node);
            /* microlink_peer_info_t.vpn_ip is host byte order. */
            char buf[16];
            ip4_hbo_to_str(pi.vpn_ip, buf, sizeof buf);
            cJSON_AddStringToObject(p, "vpn_ip", buf);
            cJSON *routes = cJSON_CreateArray();
            for (int r = 0; r < pi.subnet_route_count; r++) {
                cJSON *rt = cJSON_CreateObject();
                ip4_hbo_to_str(pi.subnet_routes[r].network, buf, sizeof buf);
                cJSON_AddStringToObject(rt, "network",    buf);
                cJSON_AddNumberToObject(rt, "prefix_len", pi.subnet_routes[r].prefix_len);
                cJSON_AddItemToArray(routes, rt);
            }
            cJSON_AddItemToObject(p, "subnet_routes", routes);
            cJSON_AddItemToArray(peers, p);
        }
    }
    cJSON_AddItemToObject(root, "peers", peers);

    /* Accepted-routes table (peer routes we've decided to honour). */
    cJSON *acc = cJSON_CreateArray();
    for (int i = 0; i < tailscale_accepted_routes_count; i++) {
        cJSON *r = cJSON_CreateObject();
        char buf[16];
        ip4_hbo_to_str(tailscale_accepted_routes[i].network, buf, sizeof buf);
        cJSON_AddStringToObject(r, "network",    buf);
        cJSON_AddNumberToObject(r, "prefix_len", tailscale_accepted_routes[i].prefix_len);
        cJSON_AddItemToArray(acc, r);
    }
    cJSON_AddItemToObject(root, "accepted_routes", acc);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_tailscale = {
    .uri      = "/api/tailscale",
    .method   = HTTP_GET,
    .handler  = tailscale_handler,
    .user_ctx = NULL,
};

static esp_err_t tailscale_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    nvs_save_errors_reset();

    /* Heap-allocate the body buffer — see network_save_handler comment. */
    size_t buf_size = 2048;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Same omit-to-keep convention as /api/network. The "settings"
     * wrapper matches the GET shape so the SPA can round-trip its
     * editable form straight back. auth_key is write-only — it's never
     * returned by the GET, and POSTing it with an empty string clears
     * the credential. */
    const cJSON *s = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(s)) {
        const cJSON *enabled = cJSON_GetObjectItem(s, "enabled");
        if (cJSON_IsBool(enabled)) {
            nvs_save_int("ts_enabled", cJSON_IsTrue(enabled) ? 1 : 0);
        }
        save_str_if_present(s, "auth_key",         "ts_authkey");
        save_str_if_present(s, "hostname",         "ts_hostname");
        save_str_if_present(s, "login_server",     "ts_login");
        save_str_if_present(s, "advertise_routes", "ts_routes");

        /* tailscale_init only reads these globals at boot, so the
         * /api/tailscale GET that immediately follows this POST would
         * still serialise the stale value. Mirror the just-saved
         * strings back into the heap-allocated globals so the next
         * read-back sees the new state without waiting for a reboot. */
        #define _TS_REFRESH_STR(json_key, global_var)                       \
            do {                                                            \
                const cJSON *_v = cJSON_GetObjectItem(s, json_key);         \
                if (cJSON_IsString(_v)) {                                   \
                    free(global_var);                                       \
                    global_var = strdup(_v->valuestring);                   \
                }                                                           \
            } while (0)
        #define _TS_REFRESH_BOOL(json_key, global_var)                      \
            do {                                                            \
                const cJSON *_v = cJSON_GetObjectItem(s, json_key);         \
                if (cJSON_IsBool(_v)) global_var = cJSON_IsTrue(_v) ? 1 : 0;\
            } while (0)
        #define _TS_REFRESH_NUM(json_key, global_var)                       \
            do {                                                            \
                const cJSON *_v = cJSON_GetObjectItem(s, json_key);         \
                if (cJSON_IsNumber(_v)) global_var = (int32_t)_v->valuedouble; \
            } while (0)

        _TS_REFRESH_STR ("auth_key",                tailscale_auth_key);
        _TS_REFRESH_STR ("hostname",                tailscale_hostname);
        _TS_REFRESH_STR ("login_server",            tailscale_login_server);
        _TS_REFRESH_STR ("advertise_routes",        tailscale_advertise_routes);
        _TS_REFRESH_BOOL("enabled",                 tailscale_enabled);
        _TS_REFRESH_NUM ("max_peers",               tailscale_max_peers);
        _TS_REFRESH_NUM ("default_derp_region",     tailscale_default_derp_region);
        _TS_REFRESH_NUM ("netcheck_threshold_ms",   tailscale_netcheck_threshold_ms);
        _TS_REFRESH_BOOL("netcheck_override",       tailscale_netcheck_override);
        _TS_REFRESH_BOOL("lan_bypass",              tailscale_lan_bypass);
        _TS_REFRESH_BOOL("accept_routes",           tailscale_accept_routes);

        #undef _TS_REFRESH_STR
        #undef _TS_REFRESH_BOOL
        #undef _TS_REFRESH_NUM
        save_int_if_present(s, "max_peers",        "ts_maxpeers");
        save_int_if_present(s, "default_derp_region",   "ts_def_derp");
        save_int_if_present(s, "netcheck_threshold_ms", "ts_nc_thr");

        const cJSON *bool_keys[][2] = {
            { cJSON_GetObjectItem(s, "netcheck_override"), (void *)"ts_nc_ovr"  },
            { cJSON_GetObjectItem(s, "lan_bypass"),        (void *)"ts_lan_bp"  },
            { cJSON_GetObjectItem(s, "accept_routes"),     (void *)"ts_acpt_rt" },
        };
        for (size_t i = 0; i < sizeof bool_keys / sizeof bool_keys[0]; i++) {
            const cJSON *v = bool_keys[i][0];
            if (cJSON_IsBool(v)) {
                nvs_save_int((const char *)bool_keys[i][1], cJSON_IsTrue(v) ? 1 : 0);
            }
        }

        /* exit_node_ip is a dotted-quad string in the JSON, stored in
         * NVS as the host-order int that tailscale_init reads back, and
         * consumed by lwip_route_hook + keepalive_start which both
         * expect host byte order. ip4addr_aton fills a.addr in network
         * byte order, so we MUST ntohl before persisting and mirroring;
         * otherwise byte-reversed comparisons miss the chosen peer and
         * the supervisor never flips netif_default. */
        const cJSON *exit_node = cJSON_GetObjectItem(s, "exit_node_ip");
        if (cJSON_IsString(exit_node)) {
            ip4_addr_t a = { 0 };
            if (exit_node->valuestring[0] == '\0' || ip4addr_aton(exit_node->valuestring, &a)) {
                uint32_t hbo = lwip_ntohl(a.addr);
                nvs_save_int("ts_exit_node", (int32_t)hbo);
                tailscale_exit_node_ip = hbo;
            }
        }
    }

    /* MTU/MSS/PMTU manager — operator can pick AUTO (peer-aware,
     * 1420 when at least one peer is direct, 1280 when only DERP) or
     * FIXED with a hand-picked MTU in [576..1500]. tailscale_mtu_set
     * persists to NVS and immediately recomputes eff_mtu / eff_mss /
     * eff_pmtu — change is live without reboot. */
    const cJSON *mtu_j = cJSON_GetObjectItem(root, "mtu");
    if (cJSON_IsObject(mtu_j)) {
        ts_mtu_state_t cur = tailscale_mtu_get();
        ts_mtu_mode_t  mode      = cur.mode;
        uint16_t       fixed_mtu = cur.fixed_mtu;
        const cJSON *m  = cJSON_GetObjectItem(mtu_j, "mode");
        const cJSON *fm = cJSON_GetObjectItem(mtu_j, "fixed_mtu");
        if (cJSON_IsNumber(m)) {
            int v = (int)m->valuedouble;
            if (v == TS_MTU_AUTO || v == TS_MTU_FIXED) mode = (ts_mtu_mode_t)v;
        }
        if (cJSON_IsNumber(fm)) {
            int v = (int)fm->valuedouble;
            if (v >= TS_MTU_MIN && v <= TS_MTU_MAX) fixed_mtu = (uint16_t)v;
        }
        tailscale_mtu_set(mode, fixed_mtu);
    }

    cJSON_Delete(root);
    /* Surface any silent NVS-out-of-space failures from the save_* path
     * in the response. The SPA renders nvs_save_error as a red toast
     * so the operator doesn't get a green "Saved" when the on-flash
     * state isn't actually changing. */
    cJSON *resp = cJSON_CreateObject();
    nvs_save_errors_attach(resp);
    cJSON_AddBoolToObject(resp, "restart_required", true);
    char *body = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{\"ok\":false}");
    free(body);
    return e;
}

/* Forward decl — the implementation is below the tailscale block but
 * we need it for the reset-identity handler that lives up here. */
static void delayed_restart_task(void *arg);

/* POST /api/tailscale/reset_identity — clears the device's stored
 * machine / wg / disco keypairs + cached peer table so the next boot
 * generates fresh ones. Use this when the control plane no longer
 * recognises the node-key (deleted on the server, or a fresh Headscale
 * DB). microlink_factory_reset() touches its own NVS namespaces — no
 * other config is affected. Restart is required because the keys are
 * read once at microlink_init. */
static esp_err_t tailscale_reset_identity_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    esp_err_t err = microlink_factory_reset();
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof resp, "{\"ok\":false,\"reason\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, resp);
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(delayed_restart_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static const httpd_uri_t uri_tailscale_reset_identity = {
    .uri      = "/api/tailscale/reset_identity",
    .method   = HTTP_POST,
    .handler  = tailscale_reset_identity_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_tailscale_save = {
    .uri      = "/api/tailscale",
    .method   = HTTP_POST,
    .handler  = tailscale_save_handler,
    .user_ctx = NULL,
};

/* Session-management forward declarations — the full implementation lives
 * further down the file, but system_handler / system_save_handler /
 * auth_setup_handler need to refer to s_session_timeout_s and a handful of
 * session_*() helpers before they're defined. C's tentative-definition rule
 * lets us put the storage class + type here; the initialiser below
 * resolves into the same object. */
static uint32_t s_session_timeout_s;
static uint32_t session_timeout_clamp(uint32_t v);
static uint32_t session_remaining_s_for_req(httpd_req_t *req);
static bool     session_alive(void);
static void     session_extend_all_alive(void);

static esp_err_t system_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        cJSON_AddStringToObject(root, "version",    desc->version);
        cJSON_AddStringToObject(root, "build_date", desc->date);
        cJSON_AddStringToObject(root, "build_time", desc->time);
        cJSON_AddStringToObject(root, "idf_ver",    desc->idf_ver);
    }

    /* Device label (the friendly name shown in the nav). */
    {
        char *name = device_name_dup();
        if (name) { cJSON_AddStringToObject(root, "device_name", name); free(name); }
    }

    /* POSIX timezone string (e.g. "CET-1CEST,M3.5.0,M10.5.0/3"). Empty
     * means UTC. Surfaced to the UI so it can render local clock times. */
    {
        char *tz = nvs_param_get_str("tz");
        if (tz) { cJSON_AddStringToObject(root, "tz", tz); free(tz); }
        else    { cJSON_AddStringToObject(root, "tz", ""); }
    }

    /* Web-session idle timeout — operator-configurable from the System
     * tab. session_remaining_s lets the SPA render a discrete countdown
     * next to the lock button without needing its own /api/auth poll. */
    cJSON_AddNumberToObject(root, "session_timeout_s",   s_session_timeout_s);
    cJSON_AddNumberToObject(root, "session_remaining_s", session_remaining_s_for_req(req));

    /* TX-power override (0 = IDF default ≈ 20 dBm, 8..84 = custom in
     * 0.25 dBm steps). Reading via the live API gives whatever was
     * actually applied, not the NVS persisted value — they match
     * unless the operator changed it without a reboot. */
    {
        int8_t live_pwr = 0;
        if (esp_wifi_get_max_tx_power(&live_pwr) == ESP_OK) {
            cJSON_AddNumberToObject(root, "tx_power", live_pwr);
        }
        uint8_t nvs_pwr = 0;
        if (nvs_param_get_u8("tx_pwr", &nvs_pwr) == ESP_OK) {
            cJSON_AddNumberToObject(root, "tx_power_nvs", nvs_pwr);
        }
    }

    /* Telemetry status — the API key is intentionally not exposed. */
    telemetry_state_t tm = telemetry_get_state();
    cJSON *t = cJSON_CreateObject();
    cJSON_AddBoolToObject  (t, "enabled",     tm.enabled);
    cJSON_AddStringToObject(t, "url",         tm.url);
    cJSON_AddNumberToObject(t, "boot_count",  tm.boot_count);
    cJSON_AddNumberToObject(t, "flash_count", tm.flash_count);
    cJSON_AddStringToObject(t, "device_hash", tm.device_hash);
    cJSON_AddNumberToObject(t, "last_send_ms", (double)tm.last_send_ms);
    cJSON_AddStringToObject(t, "last_status", tm.last_status);
    cJSON_AddItemToObject(root, "telemetry", t);

    /* Remote syslog status. */
    cJSON *sl = cJSON_CreateObject();
    bool sl_enabled = false;
    char sl_server[64] = {0};
    uint16_t sl_port = 0;
    syslog_get_config(&sl_enabled, sl_server, sizeof sl_server, &sl_port);
    cJSON_AddBoolToObject  (sl, "enabled", sl_enabled);
    cJSON_AddStringToObject(sl, "server",  sl_server);
    cJSON_AddNumberToObject(sl, "port",    sl_port);
    cJSON_AddItemToObject(root, "syslog", sl);

    /* Log tail — read into a heap buffer to keep the request handler
     * stack small. Truncated to a known size so the JSON stays bounded. */
    char *log_buf = malloc(WEB_UI_LOG_SNAPSHOT_BYTES);
    if (log_buf) {
        size_t n = log_capture_read(log_buf, WEB_UI_LOG_SNAPSHOT_BYTES - 1);
        log_buf[n] = '\0';
        cJSON_AddStringToObject(root, "log_tail", log_buf);
        free(log_buf);
    }

    /* OTA — poller state. Poll period is now a fixed 24 h, not a
     * setting; the only opt-in is auto_install + a preferred install
     * hour (-1 = ASAP). update_available drives the Status banner. */
    {
        ota_state_t os;
        ota_get_state(&os);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject  (o, "auto_install",     os.auto_install);
        cJSON_AddNumberToObject(o, "install_hour",     os.install_hour);
        cJSON_AddNumberToObject(o, "last_check",       os.last_check);
        cJSON_AddStringToObject(o, "running_version",  os.running_version);
        cJSON_AddStringToObject(o, "last_version",     os.last_version);
        cJSON_AddStringToObject(o, "release_notes",    os.release_notes);
        cJSON_AddStringToObject(o, "last_status",      os.last_status);
        cJSON_AddBoolToObject  (o, "update_available", os.update_available);
        cJSON_AddItemToObject(root, "ota", o);
    }

    /* Reset history — last 10 boots, [0] is the most recent. The recorder
     * runs at app_main() time, the coredump backfill writes hist[0].crash
     * when a panic-from-prior-boot was decoded on this boot. */
    {
        reset_history_entry_t hist[RESET_HISTORY_MAX] = {0};
        int n = reset_history_load(hist, RESET_HISTORY_MAX);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "wallclock", hist[i].wallclock);
            cJSON_AddStringToObject(e, "reason",    hist[i].reason);
            cJSON_AddStringToObject(e, "who",       hist[i].who);
            cJSON_AddStringToObject(e, "crash",     hist[i].crash);
            cJSON_AddItemToArray(arr, e);
        }
        cJSON_AddItemToObject(root, "reset_history", arr);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static const httpd_uri_t uri_system = {
    .uri      = "/api/system",
    .method   = HTTP_GET,
    .handler  = system_handler,
    .user_ctx = NULL,
};

static void delayed_restart_task(void *arg)
{
    /* Give the HTTP response a moment to flush + the TCP socket to close
     * cleanly before we yank the power. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t system_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    nvs_save_errors_reset();

    char buf[512];
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Device-name update — operator-defined label persisted under
     * NVS "dev_name", surfaced everywhere the SPA reads /api/auth/status
     * or /api/system. */
    const cJSON *dn = cJSON_GetObjectItem(root, "device_name");
    if (cJSON_IsString(dn)) {
        nvs_save_str("dev_name", dn->valuestring);
    }

    /* Timezone — POSIX string. tzset() pulls in the new rule for any
     * fresh localtime() call, but the IDF log subsystem reads TZ once
     * at boot and caches it, so the operator's main "where do my log
     * timestamps live" question only resolves after a reboot.
     * Compare against the persisted value so we only flip the
     * restart_required flag when the operator actually changed it. */
    bool tz_changed = false;
    const cJSON *tz_j = cJSON_GetObjectItem(root, "tz");
    if (cJSON_IsString(tz_j)) {
        char *cur_tz = nvs_param_get_str("tz");
        const char *cur_str = cur_tz ? cur_tz : "";
        if (strcmp(cur_str, tz_j->valuestring) != 0) {
            tz_changed = true;
        }
        free(cur_tz);
        nvs_save_str("tz", tz_j->valuestring);
        setenv("TZ", tz_j->valuestring[0] ? tz_j->valuestring : "UTC0", 1);
        tzset();
    }

    /* Web-session idle timeout (seconds). Persisted under NVS "auth_to_s"
     * and clamped server-side to [60, 28800] so a misbehaving client
     * can't lock everyone out with 0 or set a year-long window. The
     * sliding-window logic in request_authenticated() uses the new value
     * on the very next authenticated hit — no reboot needed. */
    const cJSON *st = cJSON_GetObjectItem(root, "session_timeout_s");
    if (cJSON_IsNumber(st)) {
        uint32_t v = (uint32_t)st->valuedouble;
        v = session_timeout_clamp(v);
        nvs_save_u32("auth_to_s", v);
        s_session_timeout_s = v;
        session_extend_all_alive();
    }

    /* TX-power override — clamp + persist + apply live. 0 disables the
     * override (next boot will skip the call and let the IDF default
     * stand). */
    const cJSON *tx_j = cJSON_GetObjectItem(root, "tx_power");
    if (cJSON_IsNumber(tx_j)) {
        int v = (int)tx_j->valuedouble;
        if (v < 0)  v = 0;
        if (v > 84) v = 84;
        if (v != 0 && v < 8) v = 8;        /* IDF rejects values below 8 */
        nvs_save_u8("tx_pwr", (uint8_t)v);
        if (v >= 8) esp_wifi_set_max_tx_power((int8_t)v);
    }

    /* Telemetry block — only the enabled toggle is editable from the UI.
     * The worker URL + X-Tlm-Key live in the firmware to keep them off
     * the operator's editable surface. */
    const cJSON *t = cJSON_GetObjectItem(root, "telemetry");
    if (cJSON_IsObject(t)) {
        const cJSON *en = cJSON_GetObjectItem(t, "enabled");
        if (cJSON_IsBool(en))  telemetry_set_enabled(cJSON_IsTrue(en));
    }

    /* OTA settings — auto_install toggle + preferred install_hour.
     * Polling itself is non-configurable (daily). Omitting either
     * key preserves its current value, so the SPA can PATCH one
     * field without round-tripping both. */
    const cJSON *o = cJSON_GetObjectItem(root, "ota");
    if (cJSON_IsObject(o)) {
        ota_state_t cur; ota_get_state(&cur);
        const cJSON *en = cJSON_GetObjectItem(o, "auto_install");
        const cJSON *ih = cJSON_GetObjectItem(o, "install_hour");
        bool auto_install = cJSON_IsBool(en)   ? cJSON_IsTrue(en)
                                               : cur.auto_install;
        int  install_hour = cJSON_IsNumber(ih) ? (int)ih->valuedouble
                                               : cur.install_hour;
        ota_set_settings(auto_install, install_hour);
    }

    /* Syslog block — { enabled, server, port }. Toggling enabled fires
     * the appropriate enable/disable so the vprintf hook installs or
     * tears down immediately. */
    const cJSON *sl = cJSON_GetObjectItem(root, "syslog");
    if (cJSON_IsObject(sl)) {
        const cJSON *en  = cJSON_GetObjectItem(sl, "enabled");
        const cJSON *srv = cJSON_GetObjectItem(sl, "server");
        const cJSON *prt = cJSON_GetObjectItem(sl, "port");
        if (cJSON_IsBool(en) && cJSON_IsTrue(en)) {
            const char *server = cJSON_IsString(srv) ? srv->valuestring : "";
            uint16_t    port   = cJSON_IsNumber(prt) ? (uint16_t)prt->valuedouble : 514;
            syslog_enable(server, port);
        } else if (cJSON_IsBool(en) && cJSON_IsFalse(en)) {
            syslog_disable();
        }
    }

    cJSON_Delete(root);
    /* Surface any NVS write failures from the save path — see the
     * twin block in tailscale_save_handler. */
    cJSON *resp = cJSON_CreateObject();
    nvs_save_errors_attach(resp);
    if (tz_changed) cJSON_AddBoolToObject(resp, "restart_required", true);
    char *body = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{\"ok\":false}");
    free(body);
    return e;
}

static esp_err_t system_restart_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(delayed_restart_task, "reboot", 2048, NULL, 5, NULL);
    return err;
}

static esp_err_t system_factory_reset_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    nvs_param_erase_all();
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(delayed_restart_task, "reboot", 2048, NULL, 5, NULL);
    return err;
}

/* Deliberately abort() the device for testing the panic-capture chain.
 * Auth-gated; not exposed in the SPA UI — only useful via curl from a
 * developer's workstation. After a 200 ms delay (let the JSON flush),
 * calls abort() so the IDF panic handler fires for real — coredump
 * lands in the partition, panic_print_* output funnels into the RTC
 * pre-crash ring via __wrap_panic_print_char. */
static void delayed_abort_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    abort();
}
static esp_err_t system_debug_crash_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, "{\"ok\":true,\"aborting\":true}");
    xTaskCreate(delayed_abort_task, "crash", 2048, NULL, 5, NULL);
    return err;
}
static const httpd_uri_t uri_system_debug_crash = {
    .uri = "/api/debug/crash", .method = HTTP_POST, .handler = system_debug_crash_handler,
};

static const httpd_uri_t uri_system_save = {
    .uri = "/api/system", .method = HTTP_POST, .handler = system_save_handler,
};
static const httpd_uri_t uri_system_restart = {
    .uri = "/api/system/restart", .method = HTTP_POST, .handler = system_restart_handler,
};
static const httpd_uri_t uri_system_factory_reset = {
    .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = system_factory_reset_handler,
};

/* Manual OTA firmware upload. Body is raw .bin bytes (Content-Type is
 * irrelevant to the writer); auth is required and gated here so the
 * actual ota_upload_handler can stay agnostic. */
static esp_err_t system_ota_upload_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    esp_err_t err = ota_upload_handler(req);
    if (err == ESP_OK) {
        /* Schedule a delayed reboot — same pattern as /api/system/restart
         * so the success JSON has time to flush over the socket. */
        xTaskCreate(delayed_restart_task, "reboot", 2048, NULL, 5, NULL);
    }
    return err;
}
static const httpd_uri_t uri_system_ota = {
    .uri = "/api/system/ota", .method = HTTP_POST, .handler = system_ota_upload_handler,
};

/* Operator-driven "check GitHub now" button — synchronous one-shot. */
static esp_err_t system_ota_poll_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    char status[64] = {0};
    ota_poll_now(status, sizeof status);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "ok", true);
    cJSON_AddStringToObject(root, "status", status);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{\"ok\":true}");
    free(body);
    return e;
}
static const httpd_uri_t uri_system_ota_poll = {
    .uri = "/api/system/ota/poll", .method = HTTP_POST, .handler = system_ota_poll_handler,
};

/* Operator-driven "install now" — applies the cached pending update
 * (set by the latest poll) and reboots. No-op if update_available
 * is false. */
static esp_err_t system_ota_install_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    ota_state_t cur; ota_get_state(&cur);
    if (!cur.update_available) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"reason\":\"no update available\"}");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req,
        "{\"ok\":true,\"reboot_required\":true}");
    /* Fire-and-forget so the HTTP response gets flushed before the
     * blocking download begins. ota_install_now() reboots on success. */
    xTaskCreate((TaskFunction_t)ota_install_now, "ota_inst",
                6144, NULL, 3, NULL);
    return e;
}
static const httpd_uri_t uri_system_ota_install = {
    .uri = "/api/system/ota/install", .method = HTTP_POST,
    .handler = system_ota_install_handler,
};

/* ---- Encrypted-secrets backup endpoints ---------------------------------
 * Plain-text secrets bundle. NEVER reached without a valid session, and
 * the SPA passphrase-encrypts the response BEFORE writing the backup
 * file to disk. Restore is the mirror image — SPA decrypts in the
 * browser, POSTs the plain bundle here, NVS writes happen atomically
 * through the same nvs_save_* helpers as every other config handler. */
static esp_err_t system_secrets_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* Tailscale auth key + AP password — both are write-only NVS strings
     * that the normal GET endpoints intentionally redact. */
    {
        char *s = nvs_param_get_str("ts_authkey");
        cJSON_AddStringToObject(root, "auth_key", s ? s : "");
        free(s);
    }
    {
        char *s = nvs_param_get_str("ap_passwd");
        cJSON_AddStringToObject(root, "ap_password", s ? s : "");
        free(s);
    }

    /* Full network table including PSK + EAP credentials. Mirrors the
     * wifi_network_t layout so a round-trip restore reproduces the
     * exact NVS state. */
    cJSON *nets = cJSON_CreateArray();
    int count = wifi_networks_count();
    for (int i = 0; i < count; i++) {
        wifi_network_t n;
        if (!wifi_networks_get(i, &n)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid",        n.ssid);
        cJSON_AddStringToObject(e, "password",    n.passwd);
        cJSON_AddNumberToObject(e, "eap_method",  n.eap_method);
        cJSON_AddNumberToObject(e, "eap_phase2",  n.eap_phase2);
        cJSON_AddBoolToObject  (e, "eap_cert_bundle",
                                n.eap_use_cert_bundle != 0);
        cJSON_AddStringToObject(e, "eap_identity", n.eap_identity);
        cJSON_AddStringToObject(e, "eap_username", n.eap_username);
        cJSON_AddStringToObject(e, "eap_password", n.eap_password);
        cJSON_AddItemToArray(nets, e);
    }
    cJSON_AddItemToObject(root, "networks", nets);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}
static const httpd_uri_t uri_system_secrets_get = {
    .uri = "/api/system/secrets", .method = HTTP_GET,
    .handler = system_secrets_get_handler,
};

static esp_err_t system_secrets_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    nvs_save_errors_reset();

    /* Worst-case bundle: 5 networks × ~400 B of EAP/static fields + a few
     * top-level strings. 4 KB leaves plenty of slack while still fitting
     * comfortably on the stack via malloc. */
    size_t buf_size = 4096;
    char *buf = malloc_body_buf(buf_size);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (recv_body(req, buf, buf_size, NULL) != ESP_OK) { free(buf); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    save_str_if_present(root, "auth_key",    "ts_authkey");
    save_str_if_present(root, "ap_password", "ap_passwd");

    /* Network array — only rewrite NVS when the client actually sent one,
     * so a partial restore (e.g. just the auth_key) doesn't wipe the
     * existing uplink table. */
    cJSON *nets_j = cJSON_GetObjectItem(root, "networks");
    if (cJSON_IsArray(nets_j)) {
        wifi_network_t arr[WIFI_NETWORKS_MAX];
        memset(arr, 0, sizeof arr);
        int n_out = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, nets_j) {
            if (n_out >= WIFI_NETWORKS_MAX) break;
            const cJSON *ssid_j = cJSON_GetObjectItem(e, "ssid");
            if (!cJSON_IsString(ssid_j) || !ssid_j->valuestring[0]) continue;
            wifi_network_t *n = &arr[n_out];
            strlcpy(n->ssid, ssid_j->valuestring, sizeof n->ssid);

            const cJSON *pw  = cJSON_GetObjectItem(e, "password");
            const cJSON *m   = cJSON_GetObjectItem(e, "eap_method");
            const cJSON *p2  = cJSON_GetObjectItem(e, "eap_phase2");
            const cJSON *cb  = cJSON_GetObjectItem(e, "eap_cert_bundle");
            const cJSON *id  = cJSON_GetObjectItem(e, "eap_identity");
            const cJSON *un  = cJSON_GetObjectItem(e, "eap_username");
            const cJSON *epw = cJSON_GetObjectItem(e, "eap_password");
            if (cJSON_IsString(pw))  strlcpy(n->passwd,       pw->valuestring,  sizeof n->passwd);
            if (cJSON_IsNumber(m))   n->eap_method = (uint8_t)m->valueint;
            if (cJSON_IsNumber(p2))  n->eap_phase2 = (uint8_t)p2->valueint;
            if (cJSON_IsBool(cb))    n->eap_use_cert_bundle = cJSON_IsTrue(cb) ? 1 : 0;
            if (cJSON_IsString(id))  strlcpy(n->eap_identity, id->valuestring,  sizeof n->eap_identity);
            if (cJSON_IsString(un))  strlcpy(n->eap_username, un->valuestring,  sizeof n->eap_username);
            if (cJSON_IsString(epw)) strlcpy(n->eap_password, epw->valuestring, sizeof n->eap_password);
            n->valid = 1;
            n_out++;
        }
        esp_err_t serr = wifi_networks_set_all(arr, n_out);
        if (serr != ESP_OK) nvs_save_record_err("wifi_nets", serr);
    }
    cJSON_Delete(root);

    /* Mirror the standard save-handler response: { ok, nvs_save_error? }. */
    cJSON *resp = cJSON_CreateObject();
    nvs_save_errors_attach(resp);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, out ? out : "{\"ok\":true}");
    free(out);
    return e;
}
static const httpd_uri_t uri_system_secrets_post = {
    .uri = "/api/system/secrets", .method = HTTP_POST,
    .handler = system_secrets_post_handler,
};

/* ---- Hardware diagnostics endpoint --------------------------------------
 * Wide-band snapshot of chip / heap / flash / partitions / coredump /
 * MAC / tasks. Pulls everything the Diagnostics tab shows in one
 * request so the SPA stays single-fetch. Read-only — no NVS writes,
 * no state mutation. Roughly 4-8 KB of JSON. */
static const char *part_type_str(esp_partition_type_t t, uint8_t subtype)
{
    if (t == ESP_PARTITION_TYPE_APP) {
        if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "app/factory";
        if (subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
            && subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
            static char buf[16];
            snprintf(buf, sizeof buf, "app/ota_%u",
                     (unsigned)(subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
            return buf;
        }
        if (subtype == ESP_PARTITION_SUBTYPE_APP_TEST) return "app/test";
        return "app/?";
    }
    if (t == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
            case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "data/ota";
            case ESP_PARTITION_SUBTYPE_DATA_PHY:      return "data/phy";
            case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "data/nvs";
            case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "data/coredump";
            case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "data/nvs_keys";
            case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "data/efuse";
            case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:return "data/undef";
            case ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD: return "data/esphttpd";
            case ESP_PARTITION_SUBTYPE_DATA_FAT:      return "data/fat";
            case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:   return "data/spiffs";
            default:                                  return "data/?";
        }
    }
    return "?";
}

static esp_err_t system_diag_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    /* --- A) Chip + memory ------------------------------------------------ */
    {
        cJSON *c = cJSON_CreateObject();
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        const char *model = "?";
        switch (ci.model) {
            case CHIP_ESP32:    model = "ESP32";    break;
            case CHIP_ESP32S2:  model = "ESP32-S2"; break;
            case CHIP_ESP32S3:  model = "ESP32-S3"; break;
            case CHIP_ESP32C3:  model = "ESP32-C3"; break;
            case CHIP_ESP32H2:  model = "ESP32-H2"; break;
            case CHIP_ESP32C6:  model = "ESP32-C6"; break;
            default: break;
        }
        cJSON_AddStringToObject(c, "model",    model);
        cJSON_AddNumberToObject(c, "revision", ci.revision);
        cJSON_AddNumberToObject(c, "cores",    ci.cores);
        /* features bitmask: wifi / bt / ble / 802.15.4 / embedded flash / psram */
        cJSON *feat = cJSON_CreateArray();
        if (ci.features & CHIP_FEATURE_WIFI_BGN)         cJSON_AddItemToArray(feat, cJSON_CreateString("wifi"));
        if (ci.features & CHIP_FEATURE_BT)               cJSON_AddItemToArray(feat, cJSON_CreateString("bt"));
        if (ci.features & CHIP_FEATURE_BLE)              cJSON_AddItemToArray(feat, cJSON_CreateString("ble"));
        if (ci.features & CHIP_FEATURE_IEEE802154)       cJSON_AddItemToArray(feat, cJSON_CreateString("802.15.4"));
        if (ci.features & CHIP_FEATURE_EMB_FLASH)        cJSON_AddItemToArray(feat, cJSON_CreateString("emb_flash"));
        if (ci.features & CHIP_FEATURE_EMB_PSRAM)        cJSON_AddItemToArray(feat, cJSON_CreateString("emb_psram"));
        cJSON_AddItemToObject(c, "features", feat);

        uint32_t flash_size = 0;
        if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
            cJSON_AddNumberToObject(c, "flash_size", flash_size);
        }
        uint32_t flash_id = 0;
        if (esp_flash_read_id(NULL, &flash_id) == ESP_OK) {
            cJSON_AddNumberToObject(c, "flash_jedec_id", flash_id);
        }

        size_t psram_total = esp_psram_get_size();
        cJSON_AddNumberToObject(c, "psram_total", psram_total);

        cJSON_AddItemToObject(root, "chip", c);
    }

    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "internal_total",    heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(m, "internal_free",     heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(m, "internal_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(m, "internal_largest",  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(m, "spiram_total",      heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(m, "spiram_free",       heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(m, "spiram_min_free",   heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(m, "spiram_largest",    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        cJSON_AddItemToObject(root, "memory", m);
    }

    /* --- B) Partitions --------------------------------------------------- */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *boot    = esp_ota_get_boot_partition();

        cJSON *parts = cJSON_CreateArray();
        esp_partition_iterator_t it = esp_partition_find(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            const esp_partition_t *p = esp_partition_get(it);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name",   p->label);
            cJSON_AddStringToObject(e, "kind",   part_type_str(p->type, p->subtype));
            cJSON_AddNumberToObject(e, "offset", p->address);
            cJSON_AddNumberToObject(e, "size",   p->size);
            cJSON_AddBoolToObject  (e, "running", running && p->address == running->address);
            cJSON_AddBoolToObject  (e, "bootnext", boot && p->address == boot->address);
            cJSON_AddItemToArray(parts, e);
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
        cJSON_AddItemToObject(root, "partitions", parts);
    }

    /* NVS namespace stats — uses the default "nvs" partition. */
    {
        cJSON *n = cJSON_CreateObject();
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            cJSON_AddNumberToObject(n, "used_entries",       st.used_entries);
            cJSON_AddNumberToObject(n, "free_entries",       st.free_entries);
            cJSON_AddNumberToObject(n, "total_entries",      st.total_entries);
            cJSON_AddNumberToObject(n, "namespace_count",    st.namespace_count);
        }
        cJSON_AddItemToObject(root, "nvs", n);
    }

    /* Coredump partition state. */
    {
        cJSON *cd = cJSON_CreateObject();
        size_t cd_addr = 0, cd_size = 0;
        esp_err_t cd_err = esp_core_dump_image_get(&cd_addr, &cd_size);
        cJSON_AddBoolToObject  (cd, "present", cd_err == ESP_OK);
        if (cd_err == ESP_OK) {
            cJSON_AddNumberToObject(cd, "addr", cd_addr);
            cJSON_AddNumberToObject(cd, "size", cd_size);
        }
        cJSON_AddItemToObject(root, "coredump", cd);
    }

    /* --- C) MAC addresses ------------------------------------------------ */
    {
        cJSON *macs = cJSON_CreateObject();
        uint8_t m[6]; char s[18];
        if (esp_wifi_get_mac(WIFI_IF_STA, m) == ESP_OK) {
            snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
            cJSON_AddStringToObject(macs, "sta", s);
        }
        if (esp_wifi_get_mac(WIFI_IF_AP, m) == ESP_OK) {
            snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
            cJSON_AddStringToObject(macs, "ap", s);
        }
        if (esp_read_mac(m, ESP_MAC_BT) == ESP_OK) {
            snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
            cJSON_AddStringToObject(macs, "bt", s);
        }
        cJSON_AddItemToObject(root, "mac", macs);
    }

    /* --- D) System ------------------------------------------------------- */
    {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "reset_reason", reset_reason_str());
        cJSON_AddStringToObject(s, "idf_version",  esp_get_idf_version());

        /* Shared sample_cpu_temp_c() handles install + enable + read;
         * having a second copy here used to race-fail the second install
         * with ESP_ERR_INVALID_STATE and silently drop cpu_temp_c from
         * whichever endpoint lost the race. */
        float tc = sample_cpu_temp_c();
        if (tc > -100.0f) cJSON_AddNumberToObject(s, "cpu_temp_c", tc);
        cJSON_AddItemToObject(root, "system", s);
    }

    /* --- E) FreeRTOS task list ------------------------------------------- */
    {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *arr = malloc(sizeof(TaskStatus_t) * n);
        cJSON *tasks = cJSON_CreateArray();
        if (arr) {
            UBaseType_t got = uxTaskGetSystemState(arr, n, NULL);
            for (UBaseType_t i = 0; i < got; i++) {
                cJSON *t = cJSON_CreateObject();
                cJSON_AddStringToObject(t, "name",     arr[i].pcTaskName ? arr[i].pcTaskName : "?");
                cJSON_AddNumberToObject(t, "prio",     arr[i].uxCurrentPriority);
                cJSON_AddNumberToObject(t, "stack_hwm", arr[i].usStackHighWaterMark);
                const char *st = "?";
                switch (arr[i].eCurrentState) {
                    case eRunning:   st = "running";   break;
                    case eReady:     st = "ready";     break;
                    case eBlocked:   st = "blocked";   break;
                    case eSuspended: st = "suspended"; break;
                    case eDeleted:   st = "deleted";   break;
                    case eInvalid:   st = "invalid";   break;
                }
                cJSON_AddStringToObject(t, "state", st);
                cJSON_AddItemToArray(tasks, t);
            }
            free(arr);
        }
        cJSON_AddItemToObject(root, "tasks", tasks);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body);
    free(body);
    return e;
}
static const httpd_uri_t uri_system_diag = {
    .uri = "/api/system/diag", .method = HTTP_GET,
    .handler = system_diag_handler,
};

/* ---- Session management -------------------------------------------------
 * Single in-memory session, like the OLD repo. 32-byte random token hex-
 * encoded into a cookie. The token never touches NVS — reboot logs
 * everyone out, which is the intended fail-safe for a router that may
 * be repurposed. Timeout is an idle/sliding window: every authenticated
 * request bumps the expiry to now + s_session_timeout_s, so the
 * operator only gets kicked out after that many seconds of real
 * inactivity. The window length itself is operator-configurable from
 * the System tab and persisted under NVS key "auth_to_s". */
#define WEB_UI_SESSION_TOKEN_LEN     32
#define WEB_UI_SESSION_TIMEOUT_DEF_S (30 * 60)
#define WEB_UI_SESSION_TIMEOUT_MIN_S 60
#define WEB_UI_SESSION_TIMEOUT_MAX_S (8 * 60 * 60)

/* Multi-session table. The original "one global s_session_token" model
 * kicked any earlier session as soon as anyone (operator's other tab,
 * a curl probe, this very file's smoke tests) called /api/auth/login
 * — the new login overwrote the only slot and the previous holder
 * started bouncing off the login overlay. WEB_UI_SESSION_MAX
 * concurrent slots let the operator keep a tab on phone + laptop and
 * still allow scripted diag access without logging anyone out. */
#define WEB_UI_SESSION_MAX 4

typedef struct {
    char     token[WEB_UI_SESSION_TOKEN_LEN * 2 + 1];   /* hex; "" = unused */
    uint64_t expires_us;
} web_session_t;

static web_session_t s_sessions[WEB_UI_SESSION_MAX] = {0};
static uint32_t      s_session_timeout_s = WEB_UI_SESSION_TIMEOUT_DEF_S;

static void hex_encode(const uint8_t *src, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", src[i]);
    out[len * 2] = '\0';
}

static uint32_t session_timeout_clamp(uint32_t v)
{
    if (v < WEB_UI_SESSION_TIMEOUT_MIN_S) return WEB_UI_SESSION_TIMEOUT_MIN_S;
    if (v > WEB_UI_SESSION_TIMEOUT_MAX_S) return WEB_UI_SESSION_TIMEOUT_MAX_S;
    return v;
}

static void session_timeout_load(void)
{
    uint32_t v = 0;
    if (nvs_param_get_u32("auth_to_s", &v) == ESP_OK && v) {
        s_session_timeout_s = session_timeout_clamp(v);
    } else {
        s_session_timeout_s = WEB_UI_SESSION_TIMEOUT_DEF_S;
    }
}

/* Returns the slot index of any non-expired live session, or -1 if
 * the whole table is empty. Used by anonymous endpoints that just
 * want to know "is anyone logged in right now". */
static bool session_alive(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    for (int i = 0; i < WEB_UI_SESSION_MAX; i++) {
        if (s_sessions[i].token[0] && now < s_sessions[i].expires_us) return true;
    }
    return false;
}

/* Wipe every slot. Used by password-change and password-clear so a
 * credential rotation invalidates ALL existing tabs. Regular logout
 * uses session_clear_slot(). */
static void session_clear_all(void)
{
    for (int i = 0; i < WEB_UI_SESSION_MAX; i++) {
        s_sessions[i].token[0]   = '\0';
        s_sessions[i].expires_us = 0;
    }
}

/* Locate the slot a request's ts_session cookie points at. Returns
 * the index, or -1 if no cookie / no match / matched-but-expired. */
static int session_find_for_req(httpd_req_t *req)
{
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof hdr) != ESP_OK) return -1;
    const char *p = strstr(hdr, "ts_session=");
    if (!p) return -1;
    p += strlen("ts_session=");
    uint64_t now = (uint64_t)esp_timer_get_time();
    for (int i = 0; i < WEB_UI_SESSION_MAX; i++) {
        if (!s_sessions[i].token[0])    continue;
        if (now >= s_sessions[i].expires_us) continue;
        size_t n = strlen(s_sessions[i].token);
        if (strncmp(p, s_sessions[i].token, n) == 0
            && (p[n] == '\0' || p[n] == ';')) {
            return i;
        }
    }
    return -1;
}

/* Pick a slot for a fresh session: prefer an empty one; otherwise
 * recycle the slot whose expiry is the oldest (LRU). */
static int session_pick_new_slot(void)
{
    int      oldest_idx = 0;
    uint64_t oldest_exp = UINT64_MAX;
    for (int i = 0; i < WEB_UI_SESSION_MAX; i++) {
        if (!s_sessions[i].token[0]) return i;
        if (s_sessions[i].expires_us < oldest_exp) {
            oldest_exp  = s_sessions[i].expires_us;
            oldest_idx  = i;
        }
    }
    return oldest_idx;
}

/* Forge a fresh token, install it in the slot, return a pointer to
 * the token string (still owned by the slot). */
static const char *session_create(void)
{
    int idx = session_pick_new_slot();
    uint8_t raw[WEB_UI_SESSION_TOKEN_LEN];
    esp_fill_random(raw, sizeof raw);
    hex_encode(raw, sizeof raw, s_sessions[idx].token);
    s_sessions[idx].expires_us = (uint64_t)esp_timer_get_time()
                                + (uint64_t)s_session_timeout_s * 1000000ULL;
    return s_sessions[idx].token;
}

static void session_clear_slot(int idx)
{
    if (idx < 0 || idx >= WEB_UI_SESSION_MAX) return;
    s_sessions[idx].token[0]   = '\0';
    s_sessions[idx].expires_us = 0;
}

/* Push every alive slot's expiry to (now + current timeout). Hooked
 * from the System tab's timeout-change save so a freshly-extended
 * window applies to existing tabs immediately, not just to the next
 * authenticated request. */
static void session_extend_all_alive(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    uint64_t new_exp = now + (uint64_t)s_session_timeout_s * 1000000ULL;
    for (int i = 0; i < WEB_UI_SESSION_MAX; i++) {
        if (s_sessions[i].token[0] && now < s_sessions[i].expires_us) {
            s_sessions[i].expires_us = new_exp;
        }
    }
}

/* Remaining seconds for the request's session, or 0 if none / expired. */
static uint32_t session_remaining_s_for_req(httpd_req_t *req)
{
    int idx = session_find_for_req(req);
    if (idx < 0) return 0;
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now >= s_sessions[idx].expires_us) return 0;
    return (uint32_t)((s_sessions[idx].expires_us - now) / 1000000ULL);
}

static bool request_authenticated(httpd_req_t *req)
{
    /* No password set → entire UI is open. Mirrors the OLD repo's
     * behaviour and lets first-boot show the password wizard. */
    if (!is_web_password_set()) return true;
    int idx = session_find_for_req(req);
    if (idx < 0) return false;
    /* Sliding window: every authenticated hit pushes the matching slot's
     * expiry out, so a tab the operator is actively poking never times out. */
    s_sessions[idx].expires_us = (uint64_t)esp_timer_get_time()
                                + (uint64_t)s_session_timeout_s * 1000000ULL;
    return true;
}

/* Read the operator-defined device label out of NVS, falling back to a
 * generic 'ESP32 Router' name. Caller frees the returned buffer. */
static char *device_name_dup(void)
{
    char *s = nvs_param_get_str("dev_name");
    if (s && s[0]) return s;
    free(s);
    return strdup("ESP32 Router");
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "auth_required", is_web_password_set());
    cJSON_AddBoolToObject(root, "authenticated", request_authenticated(req));
    /* Device name is fine to expose pre-auth — it's a label, not a secret. */
    char *name = device_name_dup();
    if (name) { cJSON_AddStringToObject(root, "device_name", name); free(name); }
    /* Idle-timeout window + remaining seconds so the SPA can drive the
     * discreet countdown next to the lock button without an extra round
     * trip. Both are safe pre-auth — they tell you the policy, not who's
     * logged in. */
    cJSON_AddNumberToObject(root, "session_timeout_s",   s_session_timeout_s);
    cJSON_AddNumberToObject(root, "session_remaining_s", session_remaining_s_for_req(req));
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t auth_login_handler(httpd_req_t *req)
{
    /* Cap the body at a sane size so a misbehaving client can't make us
     * allocate megabytes for a password field. */
    char buf[256];
    int total = req->content_len < (int)sizeof buf ? req->content_len : (int)sizeof buf - 1;
    int n = httpd_req_recv(req, buf, total);
    if (n <= 0) {
        if (n == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[n] = '\0';

    cJSON *body = cJSON_Parse(buf);
    cJSON *pw   = body ? cJSON_GetObjectItem(body, "password") : NULL;
    if (!cJSON_IsString(pw)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing password");
        return ESP_FAIL;
    }

    bool ok = verify_web_password(pw->valuestring);
    cJSON_Delete(body);

    if (!ok) {
        /* Don't leak whether a password is set or just wrong — same 401
         * either way. */
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorised");
        return ESP_FAIL;
    }

    const char *new_token = session_create();
    char cookie[160];
    snprintf(cookie, sizeof cookie,
             "ts_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u",
             new_token, (unsigned)s_session_timeout_s);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    /* Clear only the slot this request authenticates against — the
     * operator's other tabs (or a parallel scripted session) survive. */
    session_clear_slot(session_find_for_req(req));
    httpd_resp_set_hdr(req, "Set-Cookie", "ts_session=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_auth_status = {
    .uri = "/api/auth/status", .method = HTTP_GET,  .handler = auth_status_handler,
};
static const httpd_uri_t uri_auth_login = {
    .uri = "/api/auth/login",  .method = HTTP_POST, .handler = auth_login_handler,
};
static const httpd_uri_t uri_auth_logout = {
    .uri = "/api/auth/logout", .method = HTTP_POST, .handler = auth_logout_handler,
};

#define WEB_UI_PASSWORD_MIN_LEN 8

/* Returns NULL if the password meets the admin-policy requirements,
 * otherwise a static, human-readable error string. The same wording
 * is shown by the SPA next to the password input. */
static const char *check_password_policy(const char *pw)
{
    if (!pw) return "missing password";
    size_t len = strlen(pw);
    if (len < WEB_UI_PASSWORD_MIN_LEN) {
        return "password must be at least 8 characters";
    }
    bool lower = false, upper = false, digit = false, special = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (c >= 'a' && c <= 'z')      lower = true;
        else if (c >= 'A' && c <= 'Z') upper = true;
        else if (c >= '0' && c <= '9') digit = true;
        else if (c >= 33 && c <= 126)  special = true;   /* printable non-alnum */
    }
    if (!lower)   return "password must include a lowercase letter";
    if (!upper)   return "password must include an uppercase letter";
    if (!digit)   return "password must include a digit";
    if (!special) return "password must include a special character";
    return NULL;
}

static esp_err_t auth_setup_handler(httpd_req_t *req)
{
    /* First-boot wizard endpoint — only accessible while no password is
     * set. Once one exists, the client must use change_password (with
     * auth + the old password) instead. */
    if (is_web_password_set()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password already set");
        return ESP_FAIL;
    }

    char buf[256];
    int total = req->content_len < (int)sizeof buf ? req->content_len : (int)sizeof buf - 1;
    int n = httpd_req_recv(req, buf, total);
    if (n <= 0) {
        if (n == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[n] = '\0';

    cJSON *body = cJSON_Parse(buf);
    cJSON *pw   = body ? cJSON_GetObjectItem(body, "password") : NULL;
    const char *policy_err = check_password_policy(cJSON_IsString(pw) ? pw->valuestring : NULL);
    if (policy_err) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, policy_err);
        return ESP_FAIL;
    }

    esp_err_t err = set_web_password_hashed(pw->valuestring);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Log the new operator in straight away so the SPA can transition
     * from the wizard into the normal dashboard without a second POST. */
    const char *setup_token = session_create();
    char cookie[160];
    snprintf(cookie, sizeof cookie,
             "ts_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u",
             setup_token, (unsigned)s_session_timeout_s);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_auth_setup = {
    .uri = "/api/auth/setup", .method = HTTP_POST, .handler = auth_setup_handler,
};

static esp_err_t auth_change_password_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char buf[512];
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *body = cJSON_Parse(buf);
    const cJSON *old_pw = body ? cJSON_GetObjectItem(body, "old_password") : NULL;
    const cJSON *new_pw = body ? cJSON_GetObjectItem(body, "new_password") : NULL;
    if (!cJSON_IsString(old_pw) || !cJSON_IsString(new_pw)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing old_password or new_password");
        return ESP_FAIL;
    }

    if (!verify_web_password(old_pw->valuestring)) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "wrong old password");
        return ESP_FAIL;
    }

    const char *policy_err = check_password_policy(new_pw->valuestring);
    if (policy_err) {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, policy_err);
        return ESP_FAIL;
    }

    esp_err_t err = set_web_password_hashed(new_pw->valuestring);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Force re-login after a successful change — matches the OLD repo's
     * behaviour and means a stolen-cookie attacker stays out even if the
     * operator updates the password from a separate browser tab. Wipes
     * every slot, not just the requesting one. */
    session_clear_all();
    httpd_resp_set_hdr(req, "Set-Cookie", "ts_session=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_auth_change_password = {
    .uri = "/api/auth/change_password", .method = HTTP_POST, .handler = auth_change_password_handler,
};

static esp_err_t auth_clear_password_handler(httpd_req_t *req)
{
    /* Auth is required: only an already-logged-in operator can do this.
     * Once cleared the device reverts to first-boot wizard mode. */
    if (require_auth(req) != ESP_OK) return ESP_FAIL;
    set_web_password_hashed("");
    session_clear_all();
    httpd_resp_set_hdr(req, "Set-Cookie", "ts_session=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_auth_clear_password = {
    .uri = "/api/auth/clear_password", .method = HTTP_POST, .handler = auth_clear_password_handler,
};

/* Wrapper with the URI-handler signature (no err code) so the same
 * redirect can be both a wildcard URI handler AND a 404 fallback.
 * Wildcard match avoids the "httpd_uri: URI ... not found" WARN
 * spam the 404-only path used to emit. */
static esp_err_t http_to_https_redirect(httpd_req_t *req, httpd_err_code_t err);
static esp_err_t http_to_https_redirect_uri(httpd_req_t *req)
{
    return http_to_https_redirect(req, HTTPD_404_NOT_FOUND);
}

/* Catch-all handler bound to BOTH a slash-star wildcard URI and
 * the 404 error hook on the port-80 listener. The wildcard URI
 * silences the "URI not found" WARN; the 404 hook stays wired up
 * as a safety net for methods we don't register (HEAD, OPTIONS, ...). */
static esp_err_t http_to_https_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    char host_hdr[80];
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, sizeof host_hdr) != ESP_OK) {
        /* Compliant clients always send Host; pick a stable fallback for
         * the handful that don't (raw curl --resolve, embedded probes). */
        char *hn = nvs_param_get_str("hostname");
        strlcpy(host_hdr, (hn && hn[0]) ? hn : "esp32-router", sizeof host_hdr);
        free(hn);
    }
    /* Drop ":80" if the client included it — we want the canonical
     * https://host/path, not https://host:80/path which most browsers
     * resolve fine but some certificate-pinning libs don't. */
    char *colon = strchr(host_hdr, ':');
    if (colon) *colon = '\0';

    /* host_hdr is 80 B, req->uri is bounded by CONFIG_HTTPD_MAX_URI_LEN
     * (512 here), and the literal "https://" is 8 B + NUL. 640 covers
     * the worst case with headroom and keeps the compiler's
     * format-truncation analyser happy. */
    char location[640];
    snprintf(location, sizeof location, "https://%s%s", host_hdr, req->uri);

    httpd_resp_set_status(req, "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Redirecting to HTTPS\n");
}

void web_ui_init(void)
{
    static httpd_handle_t server = NULL;
    if (server) return;

    /* Pick up the operator-configured session idle timeout before we
     * start handing out cookies, so the very first login uses the
     * persisted Max-Age instead of the compile-time default. */
    session_timeout_load();

    /* PEM buffers handed to esp_https_server must stay valid for the
     * lifetime of the server, so they live in static module storage
     * rather than the start-server stack. */
    static unsigned char *s_https_crt = NULL;
    static unsigned char *s_https_key = NULL;
    size_t crt_len = 0, key_len = 0;
    if (https_cert_get_or_create(&s_https_crt, &crt_len,
                                  &s_https_key, &key_len) != ESP_OK) {
        ESP_LOGE(TAG, "no HTTPS cert — refusing to start web UI");
        return;
    }

    httpd_ssl_config_t conf       = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.uri_match_fn       = httpd_uri_match_wildcard;
    conf.httpd.max_uri_handlers   = 48;    /* we have 38 today + room to grow */
    conf.httpd.stack_size         = 12288; /* +50 % over default — multi-net POST
                                            * pushes cJSON parse + handler
                                            * locals past the 8 KB ceiling. */
    /* TLS handshake holds ~16-32 KB of mbedTLS state per connection,
     * so the 13-socket pool the plain-HTTP server got away with would
     * starve heap during a parallel SPA fetch storm. 6 leaves plenty
     * of room for first-paint (5-7 parallel) without blowing 200 KB
     * of heap on mbedTLS contexts alone. LRU-purge so a half-dropped
     * connection still rotates out instead of pinning a slot. */
    conf.httpd.max_open_sockets   = 6;
    conf.httpd.lru_purge_enable   = true;
    conf.transport_mode           = HTTPD_SSL_TRANSPORT_SECURE;
    conf.port_secure              = 443;
    /* The *_len fields take the buffer size INCLUDING the NUL terminator
     * for PEM-encoded material — see esp_https_server.h. */
    conf.servercert               = s_https_crt;
    conf.servercert_len           = crt_len + 1;
    conf.prvtkey_pem              = s_https_key;
    conf.prvtkey_len              = key_len + 1;

    if (httpd_ssl_start(&server, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed");
        free(s_https_crt); s_https_crt = NULL;
        free(s_https_key); s_https_key = NULL;
        return;
    }
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_network);
    httpd_register_uri_handler(server, &uri_network_save);
    httpd_register_uri_handler(server, &uri_network_scan);
    httpd_register_uri_handler(server, &uri_network_scan_result);
    httpd_register_uri_handler(server, &uri_tools_route);
    httpd_register_uri_handler(server, &uri_tools_ping);
    httpd_register_uri_handler(server, &uri_tools_trace);
    httpd_register_uri_handler(server, &uri_tools_pcap_get);
    httpd_register_uri_handler(server, &uri_tools_pcap_set);
    httpd_register_uri_handler(server, &uri_firewall);
    httpd_register_uri_handler(server, &uri_firewall_add);
    httpd_register_uri_handler(server, &uri_firewall_delete);
    httpd_register_uri_handler(server, &uri_firewall_clear);
    httpd_register_uri_handler(server, &uri_dhcp_reservations);
    httpd_register_uri_handler(server, &uri_dhcp_reservations_save);
    httpd_register_uri_handler(server, &uri_dhcp_leases);
    httpd_register_uri_handler(server, &uri_dhcp_kick);
    httpd_register_uri_handler(server, &uri_portmap);
    httpd_register_uri_handler(server, &uri_portmap_save);
    httpd_register_uri_handler(server, &uri_mac_deny);
    httpd_register_uri_handler(server, &uri_mac_deny_save);
    httpd_register_uri_handler(server, &uri_log_raw);
    httpd_register_uri_handler(server, &uri_log_clear);
    httpd_register_uri_handler(server, &uri_log_precrash);
    httpd_register_uri_handler(server, &uri_log_precrash_clear);
    httpd_register_uri_handler(server, &uri_tailscale);
    httpd_register_uri_handler(server, &uri_tailscale_save);
    httpd_register_uri_handler(server, &uri_tailscale_reset_identity);
    httpd_register_uri_handler(server, &uri_system);
    httpd_register_uri_handler(server, &uri_system_save);
    httpd_register_uri_handler(server, &uri_system_restart);
    httpd_register_uri_handler(server, &uri_system_factory_reset);
    httpd_register_uri_handler(server, &uri_system_ota);
    httpd_register_uri_handler(server, &uri_system_ota_poll);
    httpd_register_uri_handler(server, &uri_system_ota_install);
    httpd_register_uri_handler(server, &uri_system_secrets_get);
    httpd_register_uri_handler(server, &uri_system_secrets_post);
    httpd_register_uri_handler(server, &uri_system_diag);
    httpd_register_uri_handler(server, &uri_system_debug_crash);
    httpd_register_uri_handler(server, &uri_auth_status);
    httpd_register_uri_handler(server, &uri_auth_login);
    httpd_register_uri_handler(server, &uri_auth_logout);
    httpd_register_uri_handler(server, &uri_auth_setup);
    httpd_register_uri_handler(server, &uri_auth_change_password);
    httpd_register_uri_handler(server, &uri_auth_clear_password);
    ESP_LOGI(TAG, "web UI listening on :%d (HTTPS)", conf.port_secure);

    /* ---- Plain-HTTP → HTTPS redirector on port 80 -----------------------
     * Old bookmarks and curl calls aimed at http://<host>/ used to fail
     * with "connection refused" after Phase B closed port 80. Spin up a
     * tiny no-handler esp_http_server here whose only job is to 308
     * every request over to the matching https://<host>/<path> URL.
     * Adds ~1-2 KB to flash and lets the operator stop manually retyping
     * the scheme. */
    static httpd_handle_t redirect_server = NULL;
    httpd_config_t rconf      = HTTPD_DEFAULT_CONFIG();
    rconf.server_port         = 80;
    rconf.ctrl_port           = conf.httpd.ctrl_port + 1; /* avoid port clash with HTTPS handle */
    /* The original 4 KB stack ran the task to a 388 B high-water mark
     * (90.5 % consumed) under nothing more than the operator's bookmark
     * traffic, and the lone PANIC of 2026-05-24 right after Phase C
     * flash matched the classic stack-overflow signature
     * (vTaskSwitchContext, A11=0xa5a5a5a5). The httpd internals + URI
     * matcher + the 640 B Location buffer in http_to_https_redirect
     * just don't fit comfortably in 4 KB — bumped to 8 KB so the
     * peak use sits in 50 % of the slot instead of 90 %. Heap cost
     * is 4 KB DRAM, easily absorbed by the SPIRAM body-buffer move. */
    rconf.stack_size          = 8192;
    rconf.max_uri_handlers    = 4;   /* GET + POST wildcards + headroom */
    rconf.uri_match_fn        = httpd_uri_match_wildcard;
    rconf.max_open_sockets    = 3;     /* throwaway connections */
    rconf.lru_purge_enable    = true;
    if (httpd_start(&redirect_server, &rconf) == ESP_OK) {
        static const httpd_uri_t uri_redirect_get = {
            .uri = "/*", .method = HTTP_GET,  .handler = http_to_https_redirect_uri,
        };
        static const httpd_uri_t uri_redirect_post = {
            .uri = "/*", .method = HTTP_POST, .handler = http_to_https_redirect_uri,
        };
        httpd_register_uri_handler(redirect_server, &uri_redirect_get);
        httpd_register_uri_handler(redirect_server, &uri_redirect_post);
        httpd_register_err_handler(redirect_server, HTTPD_404_NOT_FOUND,
                                     http_to_https_redirect);
        ESP_LOGI(TAG, "HTTP→HTTPS redirect listening on :80");
    } else {
        ESP_LOGW(TAG, "HTTP redirect server failed to start "
                       "(non-fatal — HTTPS panel still works)");
    }
}
