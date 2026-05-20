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
#include "telemetry.h"
#include "log_capture.h"

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

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", desc ? desc->version : "?");
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* STA (uplink) — SSID, IP, RSSI when associated. */
    cJSON *sta = cJSON_CreateObject();
    cJSON_AddBoolToObject(sta, "connected", ap_connect != 0);
    wifi_ap_record_t apr;
    if (ap_connect && esp_wifi_sta_get_ap_info(&apr) == ESP_OK) {
        cJSON_AddStringToObject(sta, "ssid", (const char *)apr.ssid);
        cJSON_AddNumberToObject(sta, "rssi", apr.rssi);
    }
    esp_netif_t *sta_if = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta_if && esp_netif_get_ip_info(sta_if, &ip) == ESP_OK) {
        char buf[16];
        ip4_to_str(ip.ip.addr, buf, sizeof buf);
        cJSON_AddStringToObject(sta, "ip", buf);
    }
    cJSON_AddItemToObject(root, "sta", sta);

    /* AP (downlink) — client count + AP IP. */
    cJSON *ap = cJSON_CreateObject();
    cJSON_AddNumberToObject(ap, "clients", connect_count);
    esp_netif_t *ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_if && esp_netif_get_ip_info(ap_if, &ip) == ESP_OK) {
        char buf[16];
        ip4_to_str(ip.ip.addr, buf, sizeof buf);
        cJSON_AddStringToObject(ap, "ip", buf);
    }
    cJSON_AddItemToObject(root, "ap", ap);

    /* Tailscale (microlink) — runtime state from tailscale_config.h. */
    cJSON *ts = cJSON_CreateObject();
    cJSON_AddBoolToObject(ts, "enabled",   tailscale_enabled != 0);
    cJSON_AddBoolToObject(ts, "connected", tailscale_connected);
    if (tailscale_tunnel_ip) {
        char buf[16];
        ip4_to_str(tailscale_tunnel_ip, buf, sizeof buf);
        cJSON_AddStringToObject(ts, "tunnel_ip", buf);
    }
    cJSON_AddItemToObject(root, "tailscale", ts);

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
        char buf[16];
        ip4_to_str(tailscale_exit_node_ip, buf, sizeof buf);
        cJSON_AddStringToObject(settings, "exit_node_ip", buf);
    }
    cJSON_AddItemToObject(root, "settings", settings);

    /* Runtime state. */
    cJSON *runtime = cJSON_CreateObject();
    cJSON_AddBoolToObject(runtime, "connected", tailscale_connected);
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
            char buf[16];
            ip4_to_str(pi.vpn_ip, buf, sizeof buf);
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

#define WEB_UI_LOG_SNAPSHOT_BYTES 4096

static esp_err_t system_handler(httpd_req_t *req)
{
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

void web_ui_init(void)
{
    static httpd_handle_t server = NULL;
    if (server) return;

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size       = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_network);
    httpd_register_uri_handler(server, &uri_tailscale);
    httpd_register_uri_handler(server, &uri_system);
    ESP_LOGI(TAG, "web UI listening on :%d", config.server_port);
}
