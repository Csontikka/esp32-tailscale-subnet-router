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

#ifdef __cplusplus
extern "C" {
#endif

void netif_hooks_init(void);

#ifdef __cplusplus
}
#endif
