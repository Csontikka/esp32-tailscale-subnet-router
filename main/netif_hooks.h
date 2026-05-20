/* lwIP netif hook installer — currently just the ACL packet filter.
 *
 * Wires acl_check_packet into the STA and AP netifs' input + linkoutput
 * chains so the four ACL chains (to_esp / from_esp / to_ap / from_ap)
 * actually drop denied traffic. Idempotent; call once after WiFi has
 * started.
 *
 * Future hooks (byte counters, PCAP tap, TTL override, kill switch)
 * land here too, sharing the same install/save-original-fn pattern.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void netif_hooks_init(void);

/* Wire-byte counters — accumulated in the four hook tap points before
 * the ACL check, so they represent everything that actually hit the
 * interface (denied frames included). Read-only from the application. */
uint64_t netif_hooks_get_sta_bytes_in (void);
uint64_t netif_hooks_get_sta_bytes_out(void);
uint64_t netif_hooks_get_ap_bytes_in  (void);
uint64_t netif_hooks_get_ap_bytes_out (void);

#ifdef __cplusplus
}
#endif
