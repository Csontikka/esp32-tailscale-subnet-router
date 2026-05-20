/* Fixed MAC→IP DHCP reservation storage with optional friendly names.
 *
 * Pure in-memory table backed by a single NVS blob ("dhcp_res" under the
 * "tsr" namespace). Lookups are cache-only — the DHCP server calls
 * dhcp_reservations_lookup() from its OFFER/ACK path, so it MUST stay
 * lock-free and synchronous. NVS only touched on init and set_all.
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

#define DHCP_RESERVATIONS_MAX    16
#define DHCP_RESERVATION_NAME_LEN 32

typedef struct {
    uint8_t  mac[6];                              /* Client MAC. */
    uint8_t  _pad0[2];                            /* Align ip on 4-byte. */
    uint32_t ip;                                  /* Reserved address, network byte order. */
    char     name[DHCP_RESERVATION_NAME_LEN];     /* Optional friendly label. */
    uint8_t  valid;                               /* 1 = slot in use. */
    uint8_t  _reserved[7];                        /* On-flash padding for future fields. */
} dhcp_reservation_t;

/* Load reservations from NVS. Idempotent. */
void                dhcp_reservations_init(void);

/* Number of valid entries (0..DHCP_RESERVATIONS_MAX). */
int                 dhcp_reservations_count(void);

/* Copy entry i into *out. Returns false on out-of-range, empty slot, or NULL out. */
bool                dhcp_reservations_get(int i, dhcp_reservation_t *out);

/* Replace the entire table. count clamped, empty-MAC entries dropped. */
esp_err_t           dhcp_reservations_set_all(const dhcp_reservation_t *arr, int count);

/* Lookup helpers — return 0 / NULL when no reservation matches. */
uint32_t            dhcp_reservations_lookup(const uint8_t mac[6]);
const char         *dhcp_reservations_lookup_name_by_mac(const uint8_t mac[6]);
const char         *dhcp_reservations_lookup_name_by_ip(uint32_t ip_nbo);

#ifdef __cplusplus
}
#endif
