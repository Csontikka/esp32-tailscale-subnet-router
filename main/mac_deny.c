/* See mac_deny.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_params.h"
#include "mac_deny.h"

static const char *TAG = "mac_deny";

#define NVS_KEY "mac_deny"

static mac_deny_entry_t s_table[MAC_DENY_MAX];
static int              s_count  = 0;
static bool             s_loaded = false;

static void clear_cache(void)
{
    memset(s_table, 0, sizeof s_table);
    s_count = 0;
}

void mac_deny_init(void)
{
    clear_cache();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t expected = sizeof s_table;
        size_t actual   = expected;
        if (nvs_get_blob(nvs, NVS_KEY, s_table, &actual) == ESP_OK
            && actual == expected) {
            for (int i = 0; i < MAC_DENY_MAX; i++) {
                if (s_table[i].valid) s_count++;
            }
            ESP_LOGI(TAG, "loaded %d denied MAC(s) from NVS", s_count);
        } else {
            clear_cache();
        }
        nvs_close(nvs);
    }

    s_loaded = true;
}

int mac_deny_count(void)
{
    if (!s_loaded) mac_deny_init();
    return s_count;
}

bool mac_deny_get(int i, mac_deny_entry_t *out)
{
    if (!s_loaded) mac_deny_init();
    if (!out || i < 0 || i >= MAC_DENY_MAX) return false;
    if (!s_table[i].valid) return false;
    memcpy(out, &s_table[i], sizeof *out);
    return true;
}

static bool mac_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) if (mac[i]) return false;
    return true;
}

esp_err_t mac_deny_set_all(const mac_deny_entry_t *arr, int count)
{
    if (!arr) return ESP_ERR_INVALID_ARG;
    if (count < 0) count = 0;
    if (count > MAC_DENY_MAX) count = MAC_DENY_MAX;

    clear_cache();
    for (int i = 0; i < count; i++) {
        if (mac_is_zero(arr[i].mac)) continue;
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
        ESP_LOGI(TAG, "saved %d denied MAC(s) to NVS", s_count);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    s_loaded = true;
    return err;
}

bool mac_deny_is_blocked(const uint8_t mac[6])
{
    if (!s_loaded) mac_deny_init();
    if (!mac) return false;
    for (int i = 0; i < MAC_DENY_MAX; i++) {
        if (s_table[i].valid && memcmp(s_table[i].mac, mac, 6) == 0) {
            return true;
        }
    }
    return false;
}
