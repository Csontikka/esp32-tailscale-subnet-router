/* NAT port-forwarding (NAPT portmap) table with NVS persistence.
 *
 * Each entry maps an external (uplink-bound) TCP/UDP port to an internal
 * AP-side IP:port. lwIP's ip_portmap_add() takes care of the actual
 * SNAT/DNAT rewriting; this module owns the table, persists it to NVS,
 * and re-installs the bindings every time the STA acquires a new IP.
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

#define PORTMAP_MAX           16
#define PORTMAP_NAME_LEN      32

/* Protocol numbers — match IPPROTO_TCP / IPPROTO_UDP so we can pass
 * them straight to ip_portmap_add(). */
#define PORTMAP_PROTO_TCP     6
#define PORTMAP_PROTO_UDP     17

typedef struct {
    uint8_t  proto;                 /* 6 = TCP, 17 = UDP. */
    uint8_t  valid;                 /* 1 = slot in use. */
    uint8_t  _pad0[2];              /* Align the u16 ports. */
    uint16_t ext_port;              /* External (uplink-side) port. */
    uint16_t int_port;              /* Internal (AP-side) port. */
    uint32_t int_ip;                /* Internal AP-side IP, network byte order. */
    char     name[PORTMAP_NAME_LEN];/* Optional friendly label. */
    uint8_t  _reserved[8];          /* On-flash padding for future fields. */
} portmap_entry_t;

void               portmap_init(void);
int                portmap_count(void);
bool               portmap_get(int i, portmap_entry_t *out);
esp_err_t          portmap_set_all(const portmap_entry_t *arr, int count);

/* Refresh lwIP NAPT bindings using the current STA IP. Called from
 * main on IP_GOT_IP and after any table change. Safe to call repeatedly
 * — clears existing bindings first so duplicates don't accumulate. */
void               portmap_install_all(void);

#ifdef __cplusplus
}
#endif
