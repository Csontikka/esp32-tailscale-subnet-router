/* Multiple-uplink WiFi credential storage.
 *
 * Holds up to WIFI_NETWORKS_MAX configured networks in priority order.
 * Index 0 is the preferred network; the STA event handler rotates
 * forward on association failure and wraps around.
 *
 * Each entry carries its own static-IP block — empty strings mean
 * "use DHCP on this network". Hostname is shared across the device,
 * not per-network.
 *
 * Persistence: single NVS blob under the "tsr" namespace, key
 * "wifi_nets". Migrates from the legacy single-network keys
 * (ssid/passwd/static_ip/subnet/gateway) on first read.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_NETWORKS_MAX 5

/* WPA2-Enterprise method codes. 0 = plain PSK (no enterprise auth);
 * any non-zero value selects the corresponding EAP phase-1 method and
 * we drop the regular cfg.sta.password path. The strings the SPA
 * shows are mapped 1:1 in render_network_row() / save_network_row(). */
#define WIFI_EAP_DISABLED 0
#define WIFI_EAP_PEAP     1
#define WIFI_EAP_TTLS     2
#define WIFI_EAP_TLS      3   /* reserved — UI/config not implemented yet */

/* Phase-2 inner method for PEAP/TTLS. 0 picks the IDF default for the
 * selected outer method (MSCHAPv2 for PEAP, PAP for TTLS). */
#define WIFI_EAP_PHASE2_AUTO     0
#define WIFI_EAP_PHASE2_MSCHAPV2 1
#define WIFI_EAP_PHASE2_PAP      2
#define WIFI_EAP_PHASE2_CHAP     3
#define WIFI_EAP_PHASE2_MSCHAP   4

typedef struct {
    char    ssid[33];        /* SSID, max 32 chars + null. Empty = unused. */
    char    passwd[65];      /* PSK, max 64 chars + null. Ignored when eap_method != 0. */
    char    static_ip[16];   /* Dotted-quad string. Empty = DHCP for this network. */
    char    subnet[16];
    char    gateway[16];
    char    dns[16];         /* DNS server for this network. Empty = inherit
                              * from DHCP, or fall back to a public resolver. */
    uint8_t valid;           /* 1 = slot in use, 0 = empty (ignored). */
    uint8_t eap_method;      /* WIFI_EAP_* — 0 = plain PSK (skip enterprise path). */
    uint8_t eap_phase2;      /* WIFI_EAP_PHASE2_* — only meaningful for PEAP/TTLS. */
    uint8_t eap_use_cert_bundle; /* 1 = verify server cert with the bundled CA store. */
    uint8_t _reserved[4];    /* Reserved for future expansion without breaking the layout. */
    char    eap_identity[64];/* Outer/anonymous identity. Empty = same as username. */
    char    eap_username[64];/* Inner username for PEAP/TTLS, or cert identity for TLS. */
    char    eap_password[64];/* Inner password — meaningless for EAP-TLS. */
} wifi_network_t;

/* Load networks from NVS (or migrate legacy keys). Idempotent — safe
 * to call multiple times; subsequent calls re-read NVS. */
void       wifi_networks_init(void);

/* Number of valid configured networks (0..WIFI_NETWORKS_MAX). */
int        wifi_networks_count(void);

/* Copy the network at index i into *out. Returns false when i is out of
 * range, the slot is empty, or out is NULL. */
bool       wifi_networks_get(int i, wifi_network_t *out);

/* Replace the entire network table. arr[] must have `count` entries;
 * count is clamped to WIFI_NETWORKS_MAX and entries with empty ssid
 * are dropped before persisting. */
esp_err_t  wifi_networks_set_all(const wifi_network_t *arr, int count);

#ifdef __cplusplus
}
#endif
