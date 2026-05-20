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
        size_t expected = sizeof s_nets;
        size_t actual   = expected;
        if (nvs_get_blob(nvs, NVS_KEY, s_nets, &actual) == ESP_OK
            && actual == expected)
        {
            /* Recount valid slots — defensive in case the on-flash blob
             * has gaps from an older firmware revision. */
            for (int i = 0; i < WIFI_NETWORKS_MAX; i++) {
                if (s_nets[i].valid && s_nets[i].ssid[0]) s_count++;
            }
            nvs_close(nvs);
            s_loaded = true;
            ESP_LOGI(TAG, "loaded %d network(s) from NVS", s_count);
            return;
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
