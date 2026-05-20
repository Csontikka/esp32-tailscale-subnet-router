/* ACL firewall rule persistence.
 *
 * Saves and loads ACL rules to/from NVS flash storage. Acquires the ACL
 * lock when snapshotting the in-memory table; flash I/O happens outside
 * the lock to avoid stalling packet processing.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "acl.h"

/* Must match the NVS_NAMESPACE define in main/nvs_params.h. Duplicated
 * here to avoid a circular component dependency on main. */
#define ACL_NVS_NAMESPACE "tsr"

static const char *TAG = "acl_nvs";

esp_err_t save_acl_rules(void)
{
    /* Snapshot all ACL lists under the lock, then write to NVS outside
     * the lock to avoid blocking packet processing during flash I/O. */
    acl_entry_t snapshot[MAX_ACL_LISTS][MAX_ACL_ENTRIES];
    acl_lock();
    for (int i = 0; i < MAX_ACL_LISTS; i++) {
        acl_entry_t *rules = acl_get_rules(i);
        if (rules) memcpy(snapshot[i], rules, sizeof(acl_entry_t) * MAX_ACL_ENTRIES);
    }
    acl_unlock();

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    const char *keys[MAX_ACL_LISTS] = { "acl_0", "acl_1", "acl_2", "acl_3" };
    for (int i = 0; i < MAX_ACL_LISTS; i++) {
        err = nvs_set_blob(nvs, keys[i], snapshot[i],
                           sizeof(acl_entry_t) * MAX_ACL_ENTRIES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save list %d: %s", i, esp_err_to_name(err));
        }
    }
    err = nvs_commit(nvs);
    if (err == ESP_OK) ESP_LOGI(TAG, "ACL rules saved");
    nvs_close(nvs);
    return err;
}

esp_err_t load_acl_rules(void)
{
    /* Initialise the in-memory tables + mutex first. */
    acl_init();

    nvs_handle_t nvs;
    if (nvs_open(ACL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_OK;   /* no NVS data yet — empty tables are fine */
    }

    const char *keys[MAX_ACL_LISTS] = { "acl_0", "acl_1", "acl_2", "acl_3" };
    acl_lock();
    for (int i = 0; i < MAX_ACL_LISTS; i++) {
        acl_entry_t *rules = acl_get_rules(i);
        if (!rules) continue;

        size_t len = sizeof(acl_entry_t) * MAX_ACL_ENTRIES;
        esp_err_t err = nvs_get_blob(nvs, keys[i], rules, &len);
        if (err == ESP_OK) {
            int count = 0;
            for (int j = 0; j < MAX_ACL_ENTRIES; j++) {
                if (rules[j].valid) {
                    count++;
                    rules[j].hit_count = 0;   /* counters don't persist */
                }
            }
            if (count > 0) {
                ESP_LOGI(TAG, "loaded %d rules for %s", count, acl_get_name(i));
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load list %d: %s", i, esp_err_to_name(err));
        }
    }
    acl_unlock();
    nvs_close(nvs);
    return ESP_OK;
}
