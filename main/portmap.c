/* See portmap.h for the rationale.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "lwip/lwip_napt.h"
#include "nvs_params.h"
#include "portmap.h"

static const char *TAG = "portmap";

#define NVS_KEY "portmap"

static portmap_entry_t s_table[PORTMAP_MAX];
static int             s_count  = 0;
static bool            s_loaded = false;

static void clear_cache(void)
{
    memset(s_table, 0, sizeof s_table);
    s_count = 0;
}

void portmap_init(void)
{
    clear_cache();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t expected = sizeof s_table;
        size_t actual   = expected;
        if (nvs_get_blob(nvs, NVS_KEY, s_table, &actual) == ESP_OK
            && actual == expected) {
            for (int i = 0; i < PORTMAP_MAX; i++) {
                if (s_table[i].valid) s_count++;
            }
            ESP_LOGI(TAG, "loaded %d portmap(s) from NVS", s_count);
        } else {
            clear_cache();
        }
        nvs_close(nvs);
    }

    s_loaded = true;
}

int portmap_count(void)
{
    if (!s_loaded) portmap_init();
    return s_count;
}

bool portmap_get(int i, portmap_entry_t *out)
{
    if (!s_loaded) portmap_init();
    if (!out || i < 0 || i >= PORTMAP_MAX) return false;
    if (!s_table[i].valid) return false;
    memcpy(out, &s_table[i], sizeof *out);
    return true;
}

static bool entry_is_filled(const portmap_entry_t *e)
{
    if (!e->ext_port || !e->int_port || !e->int_ip) return false;
    if (e->proto != PORTMAP_PROTO_TCP && e->proto != PORTMAP_PROTO_UDP) return false;
    return true;
}

esp_err_t portmap_set_all(const portmap_entry_t *arr, int count)
{
    if (!arr) return ESP_ERR_INVALID_ARG;
    if (count < 0) count = 0;
    if (count > PORTMAP_MAX) count = PORTMAP_MAX;

    clear_cache();
    for (int i = 0; i < count; i++) {
        if (!entry_is_filled(&arr[i])) continue;
        memcpy(&s_table[s_count], &arr[i], sizeof s_table[0]);
        s_table[s_count].valid = 1;
        s_count++;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, NVS_KEY, s_table, sizeof s_table);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved %d portmap(s) to NVS", s_count);
        /* Re-install so the live NAPT bindings track the new table. */
        portmap_install_all();
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    s_loaded = true;
    return err;
}

void portmap_install_all(void)
{
    if (!s_loaded) portmap_init();

    /* Pull the current STA IP — without it the NAPT bind has nothing to
     * tie the external side to. Skip silently when STA isn't up yet;
     * we'll be called again from IP_GOT_IP. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (!sta || esp_netif_get_ip_info(sta, &ip_info) != ESP_OK || !ip_info.ip.addr) {
        ESP_LOGI(TAG, "STA has no IP yet; deferring portmap install");
        return;
    }
    uint32_t bind_ip = ip_info.ip.addr;

    /* Clear-then-add: avoids accumulating stale bindings when the STA
     * IP changes (e.g. on uplink rotation). */
    for (int i = 0; i < PORTMAP_MAX; i++) {
        if (s_table[i].valid) {
            ip_portmap_remove(s_table[i].proto, s_table[i].ext_port);
        }
    }
    for (int i = 0; i < PORTMAP_MAX; i++) {
        if (!s_table[i].valid) continue;
        ip_portmap_add(s_table[i].proto, bind_ip,
                       s_table[i].ext_port,
                       s_table[i].int_ip,
                       s_table[i].int_port);
    }
    ESP_LOGI(TAG, "installed %d portmap(s) on STA " IPSTR,
             s_count, IP2STR(&ip_info.ip));
}
