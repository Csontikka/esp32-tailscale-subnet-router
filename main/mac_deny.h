/* MAC denylist — blocks specific stations from associating to the AP.
 *
 * The Wi-Fi driver doesn't ship an associate-time MAC ACL on ESP-IDF,
 * so we wire it up at the application layer: every STACONNECTED event
 * looks the new client's MAC up here, and on a hit we deauth it
 * immediately. Persistent state is a single NVS blob shared with the
 * web UI so the operator can curate the list from the browser.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAC_DENY_MAX         16
#define MAC_DENY_NAME_LEN    32

typedef struct {
    uint8_t mac[6];
    uint8_t valid;
    uint8_t _pad0[1];
    char    name[MAC_DENY_NAME_LEN];   /* Operator-supplied label. */
    uint8_t _reserved[8];
} mac_deny_entry_t;

void                mac_deny_init(void);
int                 mac_deny_count(void);
bool                mac_deny_get(int i, mac_deny_entry_t *out);
esp_err_t           mac_deny_set_all(const mac_deny_entry_t *arr, int count);

/* Lock-free, sync — safe from the Wi-Fi event handler. */
bool                mac_deny_is_blocked(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
