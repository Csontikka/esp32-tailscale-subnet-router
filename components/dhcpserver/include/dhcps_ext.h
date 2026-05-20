/* Extensions over the stock ESP-IDF dhcpserver — exposed under a name
 * that does NOT collide with the IDF-shipped <dhcpserver/dhcpserver.h>.
 *
 * Reservation lookup and active-lease enumeration both live here. The
 * file is consumed by the DHCP server itself and by the application
 * code that wants to register a reservation lookup or read out leases.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirror of the upstream DHCPS_MAX_HOSTNAME_LEN — repeated locally so
 * this header can be pulled in without dragging the rest of the DHCP
 * API in transitively. */
#ifndef DHCPS_EXT_MAX_HOSTNAME_LEN
#define DHCPS_EXT_MAX_HOSTNAME_LEN 32
#endif

/* ─────────────────── Reservation lookup ─────────────────── */

/* Called from the OFFER/ACK path with the client's 6-byte MAC. Return
 * the reserved IP in network byte order, or 0 to fall back to the
 * regular pool assignment. */
typedef uint32_t (*dhcps_reservation_lookup_fn)(const uint8_t mac[6]);

/* Register (or clear, with NULL) the reservation lookup callback.
 * Callback runs on the LWIP TCP/IP task — must be lock-free + sync. */
void dhcps_set_reservation_lookup(dhcps_reservation_lookup_fn cb);

/* ─────────────────── Active lease enumeration ─────────────────── */

/* Snapshot of one active DHCP lease. The hostname carries the client-
 * supplied DHCP Option 12 value (empty when the client didn't send one). */
typedef struct {
    uint8_t  mac[6];
    uint32_t ip;                                         /* network byte order */
    uint32_t lease_timer;                                /* seconds remaining */
    char     hostname[DHCPS_EXT_MAX_HOSTNAME_LEN];
} dhcp_lease_info_t;

/* Copy up to max_leases active leases into the supplied array. Returns
 * the number written, or 0 if the server isn't running. */
int dhcps_get_active_leases(dhcp_lease_info_t *leases, int max_leases);

#ifdef __cplusplus
}
#endif
