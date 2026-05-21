/* Multiple-uplink WiFi credential storage.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_params.h"
#include "wifi_networks.h"

static const char *TAG = "wifi_nets";

#define NVS_KEY "wifi_nets"

/* Cached on first wifi_networks_init() call so reads don't hit NVS on
 * every iteration of the STA reconnect loop. */
static wifi_network_t s_nets[WIFI_NETWORKS_MAX];
static int            s_count = 0;
static bool           s_loaded = false;

/* On-flash layout used before the dns[16] field was added. Kept around so
 * boards that already had a saved blob from the older firmware can read
 * it once, then be re-written in the new format. */
typedef struct {
    char    ssid[33];
    char    passwd[65];
    char    static_ip[16];
    char    subnet[16];
    char    gateway[16];
    uint8_t valid;
    uint8_t _reserved[7];
} wifi_network_v1_t;

/* On-flash layout in use between the dns[16] addition and the EAP
 * (WPA2-Enterprise) fields. Loads as a no-EAP plain-PSK network. */
typedef struct {
    char    ssid[33];
    char    passwd[65];
    char    static_ip[16];
    char    subnet[16];
    char    gateway[16];
    char    dns[16];
    uint8_t valid;
    uint8_t _reserved[7];
} wifi_network_v2_t;

static void clear_cache(void)
{
    memset(s_nets, 0, sizeof s_nets);
    s_count = 0;
}

/* Pull the legacy single-network keys into the given temp slot. Returns
 * 1 if a non-empty SSID was found, 0 otherwise. Writes into a caller-
 * supplied buffer (not s_nets) so subsequent wifi_networks_set_all()
 * — which clears s_nets at entry — doesn't alias-zero the migration. */
static int migrate_legacy_into(wifi_network_t *n)
{
    char *ssid = nvs_param_get_str("ssid");
    if (!ssid || !ssid[0]) { free(ssid); return 0; }

    memset(n, 0, sizeof *n);
    strlcpy(n->ssid, ssid, sizeof n->ssid);
    free(ssid);

    char *pw = nvs_param_get_str("passwd");
    if (pw) { strlcpy(n->passwd, pw, sizeof n->passwd); free(pw); }

    char *ip = nvs_param_get_str("static_ip");
    if (ip) { strlcpy(n->static_ip, ip, sizeof n->static_ip); free(ip); }

    char *mask = nvs_param_get_str("subnet");
    if (mask) { strlcpy(n->subnet, mask, sizeof n->subnet); free(mask); }

    char *gw = nvs_param_get_str("gateway");
    if (gw) { strlcpy(n->gateway, gw, sizeof n->gateway); free(gw); }

    n->valid = 1;
    ESP_LOGI(TAG, "migrated legacy '%s' into slot 0", n->ssid);
    return 1;
}

void wifi_networks_init(void)
{
    clear_cache();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t actual = 0;
        if (nvs_get_blob(nvs, NVS_KEY, NULL, &actual) == ESP_OK) {
            if (actual == sizeof s_nets) {
                /* Current layout — read straight into the cache. */
                size_t sz = actual;
                if (nvs_get_blob(nvs, NVS_KEY, s_nets, &sz) == ESP_OK) {
                    for (int i = 0; i < WIFI_NETWORKS_MAX; i++) {
                        if (s_nets[i].valid && s_nets[i].ssid[0]) s_count++;
                    }
                    nvs_close(nvs);
                    s_loaded = true;
                    ESP_LOGI(TAG, "loaded %d network(s) from NVS", s_count);
                    return;
                }
            } else if (actual == sizeof(wifi_network_v1_t) * WIFI_NETWORKS_MAX) {
                /* Pre-DNS layout — read into a v1 buffer, expand each entry
                 * with an empty dns + EAP fields, then persist back in the
                 * new format so this branch only runs once. */
                wifi_network_v1_t v1[WIFI_NETWORKS_MAX] = {0};
                size_t sz = sizeof v1;
                if (nvs_get_blob(nvs, NVS_KEY, v1, &sz) == ESP_OK) {
                    for (int i = 0; i < WIFI_NETWORKS_MAX; i++) {
                        memset(&s_nets[i], 0, sizeof s_nets[i]);
                        memcpy(s_nets[i].ssid,      v1[i].ssid,      sizeof v1[i].ssid);
                        memcpy(s_nets[i].passwd,    v1[i].passwd,    sizeof v1[i].passwd);
                        memcpy(s_nets[i].static_ip, v1[i].static_ip, sizeof v1[i].static_ip);
                        memcpy(s_nets[i].subnet,    v1[i].subnet,    sizeof v1[i].subnet);
                        memcpy(s_nets[i].gateway,   v1[i].gateway,   sizeof v1[i].gateway);
                        s_nets[i].valid = v1[i].valid;
                        if (s_nets[i].valid && s_nets[i].ssid[0]) s_count++;
                    }
                    nvs_close(nvs);
                    ESP_LOGI(TAG, "migrated %d v1 network(s) → v3 (added dns + EAP fields)", s_count);
                    wifi_network_t snapshot[WIFI_NETWORKS_MAX];
                    memcpy(snapshot, s_nets, sizeof snapshot);
                    wifi_networks_set_all(snapshot, WIFI_NETWORKS_MAX);
                    s_loaded = true;
                    return;
                }
            } else if (actual == sizeof(wifi_network_v2_t) * WIFI_NETWORKS_MAX) {
                /* Pre-EAP layout — read into a v2 buffer, expand each entry
                 * with empty EAP fields, then persist back. New entries
                 * default to eap_method=WIFI_EAP_DISABLED so the
                 * configured PSK behaviour is preserved exactly. */
                wifi_network_v2_t v2[WIFI_NETWORKS_MAX] = {0};
                size_t sz = sizeof v2;
                if (nvs_get_blob(nvs, NVS_KEY, v2, &sz) == ESP_OK) {
                    for (int i = 0; i < WIFI_NETWORKS_MAX; i++) {
                        memset(&s_nets[i], 0, sizeof s_nets[i]);
                        memcpy(s_nets[i].ssid,      v2[i].ssid,      sizeof v2[i].ssid);
                        memcpy(s_nets[i].passwd,    v2[i].passwd,    sizeof v2[i].passwd);
                        memcpy(s_nets[i].static_ip, v2[i].static_ip, sizeof v2[i].static_ip);
                        memcpy(s_nets[i].subnet,    v2[i].subnet,    sizeof v2[i].subnet);
                        memcpy(s_nets[i].gateway,   v2[i].gateway,   sizeof v2[i].gateway);
                        memcpy(s_nets[i].dns,       v2[i].dns,       sizeof v2[i].dns);
                        s_nets[i].valid = v2[i].valid;
                        /* eap_method left 0 (DISABLED) — exactly the PSK behaviour we had. */
                        if (s_nets[i].valid && s_nets[i].ssid[0]) s_count++;
                    }
                    nvs_close(nvs);
                    ESP_LOGI(TAG, "migrated %d v2 network(s) → v3 (added EAP fields)", s_count);
                    wifi_network_t snapshot[WIFI_NETWORKS_MAX];
                    memcpy(snapshot, s_nets, sizeof snapshot);
                    wifi_networks_set_all(snapshot, WIFI_NETWORKS_MAX);
                    s_loaded = true;
                    return;
                }
            }
        }
        nvs_close(nvs);
    }

    /* No blob — try legacy single-key migration. Use a stack-local
     * temp so set_all's clear-then-copy doesn't alias its own input. */
    wifi_network_t tmp = {0};
    if (migrate_legacy_into(&tmp)) {
        wifi_networks_set_all(&tmp, 1);
    } else {
        ESP_LOGI(TAG, "no networks configured");
    }
    s_loaded = true;
}

int wifi_networks_count(void)
{
    if (!s_loaded) wifi_networks_init();
    return s_count;
}

bool wifi_networks_get(int i, wifi_network_t *out)
{
    if (!s_loaded) wifi_networks_init();
    if (!out || i < 0 || i >= WIFI_NETWORKS_MAX) return false;
    if (!s_nets[i].valid || !s_nets[i].ssid[0])  return false;
    memcpy(out, &s_nets[i], sizeof *out);
    return true;
}

esp_err_t wifi_networks_set_all(const wifi_network_t *arr, int count)
{
    if (!arr) return ESP_ERR_INVALID_ARG;
    if (count < 0) count = 0;
    if (count > WIFI_NETWORKS_MAX) count = WIFI_NETWORKS_MAX;

    /* Rebuild the cache: copy in valid entries (non-empty SSID), pad the
     * tail with zeros. */
    clear_cache();
    for (int i = 0; i < count; i++) {
        if (!arr[i].ssid[0]) continue;
        memcpy(&s_nets[s_count], &arr[i], sizeof s_nets[0]);
        s_nets[s_count].valid = 1;
        s_count++;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, NVS_KEY, s_nets, sizeof s_nets);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %d network(s) to NVS", s_count);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    s_loaded = true;
    return err;
}
