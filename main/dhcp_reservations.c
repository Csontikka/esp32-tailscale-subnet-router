/* DHCP reservation table — see dhcp_reservations.h for the rationale.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_params.h"
#include "dhcps_ext.h"
#include "dhcp_reservations.h"

static const char *TAG = "dhcp_res";

#define NVS_KEY "dhcp_res"

/* Cached on first init so the DHCP server's per-packet lookup path
 * never touches NVS. */
static dhcp_reservation_t s_table[DHCP_RESERVATIONS_MAX];
static int                s_count  = 0;
static bool               s_loaded = false;

static void clear_cache(void)
{
    memset(s_table, 0, sizeof s_table);
    s_count = 0;
}

void dhcp_reservations_init(void)
{
    clear_cache();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t expected = sizeof s_table;
        size_t actual   = expected;
        if (nvs_get_blob(nvs, NVS_KEY, s_table, &actual) == ESP_OK
            && actual == expected) {
            for (int i = 0; i < DHCP_RESERVATIONS_MAX; i++) {
                if (s_table[i].valid) s_count++;
            }
            ESP_LOGI(TAG, "loaded %d reservation(s) from NVS", s_count);
        } else {
            /* No blob, wrong size, or read failed — start empty. */
            clear_cache();
        }
        nvs_close(nvs);
    }

    /* Plug the lookup into the wrapped DHCP server so the next OFFER/ACK
     * round-trip honours the table. Safe to call multiple times — the
     * server just overwrites the function pointer. */
    dhcps_set_reservation_lookup(dhcp_reservations_lookup);

    s_loaded = true;
}

int dhcp_reservations_count(void)
{
    if (!s_loaded) dhcp_reservations_init();
    return s_count;
}

bool dhcp_reservations_get(int i, dhcp_reservation_t *out)
{
    if (!s_loaded) dhcp_reservations_init();
    if (!out || i < 0 || i >= DHCP_RESERVATIONS_MAX) return false;
    if (!s_table[i].valid) return false;
    memcpy(out, &s_table[i], sizeof *out);
    return true;
}

static bool mac_is_zero(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) if (mac[i]) return false;
    return true;
}

esp_err_t dhcp_reservations_set_all(const dhcp_reservation_t *arr, int count)
{
    if (!arr) return ESP_ERR_INVALID_ARG;
    if (count < 0) count = 0;
    if (count > DHCP_RESERVATIONS_MAX) count = DHCP_RESERVATIONS_MAX;

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
        ESP_LOGI(TAG, "saved %d reservation(s) to NVS", s_count);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    s_loaded = true;
    return err;
}

uint32_t dhcp_reservations_lookup(const uint8_t mac[6])
{
    if (!s_loaded) dhcp_reservations_init();
    if (!mac) return 0;
    for (int i = 0; i < DHCP_RESERVATIONS_MAX; i++) {
        if (s_table[i].valid && memcmp(s_table[i].mac, mac, 6) == 0) {
            return s_table[i].ip;
        }
    }
    return 0;
}

const char *dhcp_reservations_lookup_name_by_mac(const uint8_t mac[6])
{
    if (!s_loaded) dhcp_reservations_init();
    if (!mac) return NULL;
    for (int i = 0; i < DHCP_RESERVATIONS_MAX; i++) {
        if (s_table[i].valid
            && memcmp(s_table[i].mac, mac, 6) == 0
            && s_table[i].name[0]) {
            return s_table[i].name;
        }
    }
    return NULL;
}

const char *dhcp_reservations_lookup_name_by_ip(uint32_t ip_nbo)
{
    if (!s_loaded) dhcp_reservations_init();
    if (!ip_nbo) return NULL;
    for (int i = 0; i < DHCP_RESERVATIONS_MAX; i++) {
        if (s_table[i].valid
            && s_table[i].ip == ip_nbo
            && s_table[i].name[0]) {
            return s_table[i].name;
        }
    }
    return NULL;
}
