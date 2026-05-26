/* Tailscale (microlink) manager — settings load and lifecycle.
 *
 * Phase 1.5b: connect path drives the public microlink_config_t with
 * auth_key, device_name, max_peers, ctrl_host (Headscale support), and
 * advertise_routes (Hostinfo.RoutableIPs subnet route advertisement).
 * The full set of NVS-backed values reaches microlink through this
 * struct; no internal NVS bridging needed.
 */

#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "microlink.h"
#include "sdlog.h"
#include "tailscale_config.h"
#include "nvs_params.h"
#include "esp_sntp.h"

/* Local helper: load a string from NVS, defaulting to an empty heap
 * buffer when the key isn't there. Other modules assume non-NULL. */
static char *nvs_str_or_empty(const char *key)
{
    char *s = nvs_param_get_str(key);
    return s ? s : strdup("");
}

static const char *TAG = "tailscale_mgr";

int32_t tailscale_enabled = 0;
char* tailscale_auth_key = NULL;
char* tailscale_hostname = NULL;
char* tailscale_login_server = NULL;
char* tailscale_advertise_routes = NULL;
int32_t tailscale_max_peers = 16;
uint32_t tailscale_exit_node_ip = 0;
int32_t tailscale_netcheck_override = 1;          /* default: allow netcheck override */
int32_t tailscale_netcheck_threshold_ms = 100;    /* default: 100 ms hysteresis (sticky — home routers rarely beat Frankfurt enough to be worth switching) */
int32_t tailscale_default_derp_region = 0;        /* 0 = unset → Frankfurt fallback in microlink */
int32_t tailscale_lan_bypass = 1;                  /* 1 = exit-node lets LAN egress via STA, like tailscale --exit-node-allow-lan-access */
int32_t tailscale_accept_routes = 0;               /* 0 = default; 1 = honour subnet routes advertised by peers (tailscale --accept-routes) */

/* Accepted-routes table — rebuilt by route_supervisor_task. Stored in host
 * byte order to match the route-hook arithmetic. */
tailscale_accepted_route_t tailscale_accepted_routes[TAILSCALE_ACCEPTED_ROUTES_MAX];
int tailscale_accepted_routes_count = 0;

bool tailscale_connected = false;
uint32_t tailscale_tunnel_ip = 0;

static uint32_t tailscale_subnet_ip = 0;
static uint32_t tailscale_subnet_mask = 0;

/* Microlink instance owned by this module. */
static microlink_t *s_microlink = NULL;

/* Start SNTP if it has not been started yet.  Called by tailscale_connect_task
 * (before the WireGuard handshake needs real wall-clock time) and by the WiFi
 * STA-got-IP event handler in esp32_nat_router.c. */
void init_sntp_if_needed(void)
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void tailscale_init(void)
{
    int32_t v = 0;
    if (nvs_param_get_int("ts_enabled", &v) == ESP_OK) {
        tailscale_enabled = v;
    }
    tailscale_auth_key         = nvs_str_or_empty("ts_authkey");
    tailscale_hostname         = nvs_str_or_empty("ts_hostname");
    tailscale_login_server     = nvs_str_or_empty("ts_login");
    tailscale_advertise_routes = nvs_str_or_empty("ts_routes");
    if (nvs_param_get_int("ts_maxpeers", &v) == ESP_OK && v >= 1 && v <= 64) {
        tailscale_max_peers = v;
    }
    /* Exit-node IP stored as i32 in NVS. Tailnet CGNAT (100.64.0.0/10) fits
     * comfortably below 2^31 so the signed/unsigned reinterpret is safe. */
    if (nvs_param_get_int("ts_exit_node", &v) == ESP_OK) {
        tailscale_exit_node_ip = (uint32_t)v;
    }
    if (nvs_param_get_int("ts_nc_ovr", &v) == ESP_OK) {
        tailscale_netcheck_override = (v ? 1 : 0);
    }
    if (nvs_param_get_int("ts_nc_thr", &v) == ESP_OK && v >= 0 && v <= 5000) {
        tailscale_netcheck_threshold_ms = v;
    }
    if (nvs_param_get_int("ts_def_derp", &v) == ESP_OK && v >= 0 && v <= 65535) {
        tailscale_default_derp_region = v;
    }
    if (nvs_param_get_int("ts_lan_bp", &v) == ESP_OK) {
        tailscale_lan_bypass = (v ? 1 : 0);
    }
    if (nvs_param_get_int("ts_acpt_rt", &v) == ESP_OK) {
        tailscale_accept_routes = (v ? 1 : 0);
    }
    ESP_LOGI(TAG, "Config loaded (enabled=%ld, max_peers=%ld, login_server=%s, exit_node=%lu.%lu.%lu.%lu)",
             (long)tailscale_enabled, (long)tailscale_max_peers,
             (tailscale_login_server && tailscale_login_server[0]) ? tailscale_login_server : "<saas>",
             (unsigned long)((tailscale_exit_node_ip >> 24) & 0xFF),
             (unsigned long)((tailscale_exit_node_ip >> 16) & 0xFF),
             (unsigned long)((tailscale_exit_node_ip >> 8) & 0xFF),
             (unsigned long)(tailscale_exit_node_ip & 0xFF));
}

struct microlink_s *tailscale_get_microlink(void)
{
    return s_microlink;
}

void tailscale_set_subnet(uint32_t ip, uint32_t mask)
{
    tailscale_subnet_ip = ip;
    tailscale_subnet_mask = mask;
}

bool tailscale_in_subnet(uint32_t ip)
{
    if (tailscale_subnet_mask == 0) return false;
    return (ip & tailscale_subnet_mask) == tailscale_subnet_ip;
}

esp_err_t tailscale_connect(void)
{
    if (!tailscale_enabled) {
        ESP_LOGI(TAG, "Tailscale not enabled");
        return ESP_ERR_INVALID_STATE;
    }
    if (!tailscale_auth_key || !tailscale_auth_key[0]) {
        ESP_LOGE(TAG, "Missing auth key (ts_authkey)");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_microlink) {
        ESP_LOGW(TAG, "Already initialized; tearing down prior instance first");
        microlink_stop(s_microlink);
        microlink_destroy(s_microlink);
        s_microlink = NULL;
    }

    microlink_config_t cfg = {
        .auth_key = tailscale_auth_key,
        .device_name = (tailscale_hostname && tailscale_hostname[0]) ? tailscale_hostname : NULL,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = (uint8_t)tailscale_max_peers,
        .wifi_tx_power_dbm = 0,
        .priority_peer_ip = 0,
        .exit_node_ip = tailscale_exit_node_ip,
        .disco_heartbeat_ms = 0,
        .stun_interval_ms = 0,
        .ctrl_watchdog_ms = 0,
        .ctrl_host = (tailscale_login_server && tailscale_login_server[0]) ? tailscale_login_server : NULL,
        .advertise_routes = (tailscale_advertise_routes && tailscale_advertise_routes[0]) ? tailscale_advertise_routes : NULL,
        .netcheck_override_enabled = (tailscale_netcheck_override != 0),
        .netcheck_override_threshold_ms = (uint32_t)tailscale_netcheck_threshold_ms,
        .preferred_derp_region = (uint16_t)tailscale_default_derp_region,
    };

    s_microlink = microlink_init(&cfg);
    if (!s_microlink) {
        ESP_LOGE(TAG, "microlink_init failed");
        return ESP_FAIL;
    }

    /* Hand the SD flight-recorder the handle so its SNAP line can sample
     * task states + DERP heartbeat age — the wedge-catching signals. */
    sdlog_set_microlink(s_microlink);

    esp_err_t err = microlink_start(s_microlink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed: %s", esp_err_to_name(err));
        microlink_destroy(s_microlink);
        s_microlink = NULL;
        return err;
    }

    tailscale_connected = true;
    ESP_LOGI(TAG, "Tailscale started (device=%s, ctrl=%s, routes=%s, max_peers=%d)",
             cfg.device_name ? cfg.device_name : "<auto>",
             cfg.ctrl_host ? cfg.ctrl_host : "<saas>",
             (cfg.advertise_routes && cfg.advertise_routes[0]) ? cfg.advertise_routes : "<none>",
             cfg.max_peers);
    return ESP_OK;
}

void tailscale_disconnect(void)
{
    tailscale_connected = false;
    tailscale_tunnel_ip = 0;
    if (s_microlink) {
        microlink_stop(s_microlink);
        microlink_destroy(s_microlink);
        s_microlink = NULL;
    }
    ESP_LOGI(TAG, "Tailscale stopped");
}

bool tailscale_is_connected(void)
{
    if (!s_microlink) return false;
    bool connected = microlink_is_connected(s_microlink);
    if (connected) {
        /* microlink_get_vpn_ip returns host byte order; cache as network byte order
         * to match the existing VPN tunnel-IP convention used by netif hooks. */
        uint32_t host_ip = microlink_get_vpn_ip(s_microlink);
        if (host_ip) {
            tailscale_tunnel_ip = htonl(host_ip);
        }
    }
    return connected;
}

void tailscale_connect_task(void *pvParameters)
{
    init_sntp_if_needed();

    /* Microlink's Noise handshake is timestamped; we need real wall-clock
     * before the first registration attempt, same as WireGuard's TAI64N. */
    const int max_retry = 60;
    int retry = 0;
    time_t now = 0;
    while (retry < max_retry) {
        time(&now);
        if (now > 1577836800) break;
        if (retry % 4 == 0) {
            ESP_LOGI(TAG, "Waiting for SNTP time sync... (%d/%ds)", retry / 2, max_retry / 2);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    if (now > 1577836800) {
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP timeout after %ds, attempting Tailscale connect anyway", max_retry / 2);
    }

    tailscale_connect();
    vTaskDelete(NULL);
}
