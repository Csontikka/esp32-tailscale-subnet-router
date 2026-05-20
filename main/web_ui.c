/* Single-page web UI server.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
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
#include "nvs_params.h"
#include "microlink.h"
#include "lwip_route_hook.h"
#include "acl.h"
#include "syslog_client.h"
#include "telemetry.h"
#include "log_capture.h"
#include "web_password.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Globals owned by main.c — link status flags rendered in /api/status. */
extern int ap_connect;
extern int connect_count;

static const char *TAG = "web_ui";

/* index.html is generated as a C source by main/CMakeLists.txt
 * (file(READ HEX) → const char index_html_start[]). The PIO build
 * wrapper can't be trusted to assemble .S files emitted by ESP-IDF's
 * EMBED_TXTFILES / target_add_binary_data — see the bug write-up in
 * main/CMakeLists.txt above the file(READ ...) call. */
extern const char   index_html_start[];
extern const size_t index_html_len;

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_len);
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
static bool request_authenticated(httpd_req_t *req);
static void ip4_hbo_to_str(uint32_t hbo, char *out, size_t out_size);

static esp_err_t require_auth(httpd_req_t *req)
{
    if (request_authenticated(req)) return ESP_OK;
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "auth required");
    return ESP_FAIL;
}

/* Translate esp_reset_reason() into the short string the SPA renders. */
static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "POWERON";
        case ESP_RST_EXT:      return "EXT";
        case ESP_RST_SW:       return "SW";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "UNKNOWN";
    }
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
    }
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
    cJSON_AddItemToObject(root, "ap", ap);

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
    /* Peer count summary — full peer table lives at /api/tailscale. */
    int online = 0, total = 0;
    struct microlink_s *ml = tailscale_get_microlink();
    if (ml) {
        total = microlink_get_peer_count(ml);
        for (int i = 0; i < total; i++) {
            microlink_peer_info_t pi;
            if (microlink_get_peer_info(ml, i, &pi) == ESP_OK && pi.online) online++;
        }
    }
    cJSON_AddNumberToObject(ts, "peers_online", online);
    cJSON_AddNumberToObject(ts, "peers_total",  total);
    cJSON_AddItemToObject(root, "tailscale", ts);

    /* Telemetry summary — full counters in /api/system. */
    telemetry_state_t tm = telemetry_get_state();
    cJSON *tlm = cJSON_CreateObject();
    cJSON_AddStringToObject(tlm, "status", tm.enabled ? "ok" : "off");
    cJSON_AddNumberToObject(tlm, "boot_count",  tm.boot_count);
    cJSON_AddNumberToObject(tlm, "flash_count", tm.flash_count);
    cJSON_AddItemToObject(root, "telemetry", tlm);

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

    /* STA — upstream WiFi credentials live in NVS but the password is
     * intentionally never serialised. Static IP fields stay empty for
     * the DHCP case. */
    cJSON *sta = cJSON_CreateObject();
    add_nvs_string(sta, "ssid",     "ssid");
    add_nvs_string(sta, "hostname", "hostname");

    cJSON *static_ip = cJSON_CreateObject();
    add_nvs_string(static_ip, "ip",   "static_ip");
    add_nvs_string(static_ip, "mask", "subnet");
    add_nvs_string(static_ip, "gw",   "gateway");
    cJSON_AddItemToObject(sta, "static_ip", static_ip);
    cJSON_AddItemToObject(root, "sta", sta);

    /* AP — same exclusion for the password. */
    cJSON *ap = cJSON_CreateObject();
    add_nvs_string(ap, "ssid", "ap_ssid");
    int32_t channel = 0;
    if (nvs_param_get_int("ap_channel", &channel) == ESP_OK && channel > 0) {
        cJSON_AddNumberToObject(ap, "channel", channel);
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
static void save_str_if_present(const cJSON *obj, const char *json_key, const char *nvs_key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, json_key);
    if (cJSON_IsString(v)) nvs_param_set_str(nvs_key, v->valuestring);
}

static void save_int_if_present(const cJSON *obj, const char *json_key, const char *nvs_key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, json_key);
    if (cJSON_IsNumber(v)) nvs_param_set_int(nvs_key, (int32_t)v->valuedouble);
}

static esp_err_t network_save_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    char buf[1024];
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* STA — { ssid, password (only when changing), hostname }. The SPA
     * omits the password field when leaving it unchanged. */
    cJSON *sta = cJSON_GetObjectItem(root, "sta");
    if (cJSON_IsObject(sta)) {
        save_str_if_present(sta, "ssid",     "ssid");
        save_str_if_present(sta, "password", "passwd");
        save_str_if_present(sta, "hostname", "hostname");
    }

    /* AP — same omit-to-keep rule for the password. */
    cJSON *ap = cJSON_GetObjectItem(root, "ap");
    if (cJSON_IsObject(ap)) {
        save_str_if_present(ap, "ssid",     "ap_ssid");
        save_str_if_present(ap, "password", "ap_passwd");
        save_int_if_present(ap, "channel",  "ap_channel");
    }

    /* Static IP — passing an empty string for any of the three fields
     * clears that key, which the STA init path interprets as DHCP. */
    cJSON *static_ip = cJSON_GetObjectItem(root, "static_ip");
    if (cJSON_IsObject(static_ip)) {
        save_str_if_present(static_ip, "ip",   "static_ip");
        save_str_if_present(static_ip, "mask", "subnet");
        save_str_if_present(static_ip, "gw",   "gateway");
    }

    cJSON_Delete(root);

    /* Network changes need a reboot to apply — the SPA shows the user
     * the prompt; we just signal it here. */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
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

static esp_err_t network_scan_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 200 } },
    };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;

    wifi_ap_record_t *recs = NULL;
    if (n) {
        recs = calloc(n, sizeof *recs);
        if (!recs) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        esp_wifi_scan_get_ap_records(&n, recs);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (uint16_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid",    (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(e, "rssi",    recs[i].rssi);
        cJSON_AddNumberToObject(e, "channel", recs[i].primary);
        cJSON_AddStringToObject(e, "auth",    wifi_authmode_str(recs[i].authmode));
        cJSON_AddItemToArray(arr, e);
    }
    free(recs);
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

static esp_err_t tools_route_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) return ESP_FAIL;

    /* Parse ?dst=<ipv4>. The querystring is read into a stack buffer so
     * we don't drag malloc into this hot debug path. */
    char query[64];
    char dst_str[32];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK
        || httpd_query_key_value(query, "dst", dst_str, sizeof dst_str) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing dst");
        return ESP_FAIL;
    }

    ip4_addr_t a;
    if (!ip4addr_aton(dst_str, &a)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid IPv4");
        return ESP_FAIL;
    }
    /* route_explain takes host byte order. */
    uint32_t dst_hbo = lwip_ntohl(a.addr);

    char netif_name[16] = {0};
    char reason[160]    = {0};
    route_explain(dst_hbo, netif_name, sizeof netif_name, reason, sizeof reason);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dst",    dst_str);
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
    save_acl_rules();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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
    save_acl_rules();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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
    save_acl_rules();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

    /* Settings — auth_key is intentionally never serialised. */
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddBoolToObject  (settings, "enabled",                 tailscale_enabled != 0);
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
    cJSON_AddItemToObject(root, "runtime", runtime);

    /* Peers — empty array when microlink isn't running. */
    cJSON *peers = cJSON_CreateArray();
    struct microlink_s *ml = tailscale_get_microlink();
    if (ml) {
        int n = microlink_get_peer_count(ml);
        for (int i = 0; i < n; i++) {
            microlink_peer_info_t pi;
            if (microlink_get_peer_info(ml, i, &pi) != ESP_OK) continue;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "hostname",     pi.hostname);
            cJSON_AddBoolToObject  (p, "online",       pi.online);
            cJSON_AddBoolToObject  (p, "direct_path",  pi.direct_path);
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

    char buf[2048];   /* generous: auth_key alone can run to ~120 chars. */
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
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
            nvs_param_set_int("ts_enabled", cJSON_IsTrue(enabled) ? 1 : 0);
        }
        save_str_if_present(s, "auth_key",         "ts_authkey");
        save_str_if_present(s, "hostname",         "ts_hostname");
        save_str_if_present(s, "login_server",     "ts_login");
        save_str_if_present(s, "advertise_routes", "ts_routes");
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
                nvs_param_set_int((const char *)bool_keys[i][1], cJSON_IsTrue(v) ? 1 : 0);
            }
        }

        /* exit_node_ip is a dotted-quad string in the JSON, stored in
         * NVS as the host-order int that tailscale_init reads back. */
        const cJSON *exit_node = cJSON_GetObjectItem(s, "exit_node_ip");
        if (cJSON_IsString(exit_node)) {
            ip4_addr_t a = { 0 };
            if (exit_node->valuestring[0] == '\0' || ip4addr_aton(exit_node->valuestring, &a)) {
                nvs_param_set_int("ts_exit_node", (int32_t)a.addr);
            }
        }
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
}

static const httpd_uri_t uri_tailscale_save = {
    .uri      = "/api/tailscale",
    .method   = HTTP_POST,
    .handler  = tailscale_save_handler,
    .user_ctx = NULL,
};

#define WEB_UI_LOG_SNAPSHOT_BYTES 4096

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

    char buf[512];
    if (recv_body(req, buf, sizeof buf, NULL) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Telemetry block — { enabled, url, key }. Empty key is allowed and
     * means "keep the default key" inside telemetry_set_key. */
    const cJSON *t = cJSON_GetObjectItem(root, "telemetry");
    if (cJSON_IsObject(t)) {
        const cJSON *en = cJSON_GetObjectItem(t, "enabled");
        if (cJSON_IsBool(en))  telemetry_set_enabled(cJSON_IsTrue(en));
        const cJSON *url = cJSON_GetObjectItem(t, "url");
        if (cJSON_IsString(url)) telemetry_set_url(url->valuestring);
        const cJSON *key = cJSON_GetObjectItem(t, "key");
        if (cJSON_IsString(key)) telemetry_set_key(key->valuestring);
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
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

static const httpd_uri_t uri_system_save = {
    .uri = "/api/system", .method = HTTP_POST, .handler = system_save_handler,
};
static const httpd_uri_t uri_system_restart = {
    .uri = "/api/system/restart", .method = HTTP_POST, .handler = system_restart_handler,
};
static const httpd_uri_t uri_system_factory_reset = {
    .uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = system_factory_reset_handler,
};

/* ---- Session management -------------------------------------------------
 * Single in-memory session, like the OLD repo. 32-byte random token hex-
 * encoded into a cookie that expires after 30 min of inactivity. The
 * token never touches NVS — reboot logs everyone out, which is the
 * intended fail-safe for a router that may be repurposed. */
#define WEB_UI_SESSION_TOKEN_LEN  32
#define WEB_UI_SESSION_TIMEOUT_S  (30 * 60)

static char     s_session_token[WEB_UI_SESSION_TOKEN_LEN * 2 + 1] = {0};
static uint64_t s_session_expires_us = 0;

static void hex_encode(const uint8_t *src, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", src[i]);
    out[len * 2] = '\0';
}

static bool session_alive(void)
{
    return s_session_token[0] && (uint64_t)esp_timer_get_time() < s_session_expires_us;
}

static void session_clear(void)
{
    s_session_token[0]   = '\0';
    s_session_expires_us = 0;
}

static void session_create(void)
{
    uint8_t raw[WEB_UI_SESSION_TOKEN_LEN];
    esp_fill_random(raw, sizeof raw);
    hex_encode(raw, sizeof raw, s_session_token);
    s_session_expires_us = (uint64_t)esp_timer_get_time()
                         + (uint64_t)WEB_UI_SESSION_TIMEOUT_S * 1000000ULL;
}

static bool request_authenticated(httpd_req_t *req)
{
    /* No password set → entire UI is open. Mirrors the OLD repo's
     * behaviour and lets first-boot show the password wizard. */
    if (!is_web_password_set()) return true;
    if (!session_alive())       return false;

    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof hdr) != ESP_OK) {
        return false;
    }
    const char *p = strstr(hdr, "ts_session=");
    if (!p) return false;
    p += strlen("ts_session=");
    size_t n = strlen(s_session_token);
    return strncmp(p, s_session_token, n) == 0 && (p[n] == '\0' || p[n] == ';');
}

static esp_err_t auth_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "auth_required", is_web_password_set());
    cJSON_AddBoolToObject(root, "authenticated", request_authenticated(req));
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

    session_create();
    char cookie[160];
    snprintf(cookie, sizeof cookie,
             "ts_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d",
             s_session_token, WEB_UI_SESSION_TIMEOUT_S);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t auth_logout_handler(httpd_req_t *req)
{
    session_clear();
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
    session_create();
    char cookie[160];
    snprintf(cookie, sizeof cookie,
             "ts_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%d",
             s_session_token, WEB_UI_SESSION_TIMEOUT_S);
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
     * operator updates the password from a separate browser tab. */
    session_clear();
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
    session_clear();
    httpd_resp_set_hdr(req, "Set-Cookie", "ts_session=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t uri_auth_clear_password = {
    .uri = "/api/auth/clear_password", .method = HTTP_POST, .handler = auth_clear_password_handler,
};

void web_ui_init(void)
{
    static httpd_handle_t server = NULL;
    if (server) return;

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 32;   /* we have 21 today + room to grow */
    config.stack_size       = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_network);
    httpd_register_uri_handler(server, &uri_network_save);
    httpd_register_uri_handler(server, &uri_network_scan);
    httpd_register_uri_handler(server, &uri_tools_route);
    httpd_register_uri_handler(server, &uri_firewall);
    httpd_register_uri_handler(server, &uri_firewall_add);
    httpd_register_uri_handler(server, &uri_firewall_delete);
    httpd_register_uri_handler(server, &uri_firewall_clear);
    httpd_register_uri_handler(server, &uri_tailscale);
    httpd_register_uri_handler(server, &uri_tailscale_save);
    httpd_register_uri_handler(server, &uri_system);
    httpd_register_uri_handler(server, &uri_system_save);
    httpd_register_uri_handler(server, &uri_system_restart);
    httpd_register_uri_handler(server, &uri_system_factory_reset);
    httpd_register_uri_handler(server, &uri_auth_status);
    httpd_register_uri_handler(server, &uri_auth_login);
    httpd_register_uri_handler(server, &uri_auth_logout);
    httpd_register_uri_handler(server, &uri_auth_setup);
    httpd_register_uri_handler(server, &uri_auth_change_password);
    httpd_register_uri_handler(server, &uri_auth_clear_password);
    ESP_LOGI(TAG, "web UI listening on :%d", config.server_port);
}
