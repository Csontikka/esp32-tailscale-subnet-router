/* DNS forwarder for AP clients.
 *
 * The ESP binds a UDP socket on the AP-side IP:53 and forwards every
 * query it receives to the configured upstream resolver, then sends the
 * response back to the originating client. Compared to handing the
 * upstream DNS to clients directly via DHCP Option 6, this protects AP
 * clients from stale-cache problems when the STA uplink (and hence the
 * effective upstream resolver) changes — clients always send queries to
 * the ESP and the ESP picks the live upstream at forward time.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the relay: spawns the forwarder task and loads enable +
 * upstream-override from NVS. Idempotent. The task self-pauses on
 * recvfrom while disabled, so the call is cheap when the operator
 * leaves the feature off. */
void dns_relay_init(void);

/* Enable / disable the relay at runtime (mirrored to NVS). When off,
 * the forwarder closes its bind socket; AP clients see no listener on
 * 53/udp and DHCP should hand out the direct upstream instead. */
void dns_relay_set_enabled(bool on);
bool dns_relay_is_enabled(void);

/* True iff the forwarder task is bound and ready to serve queries.
 * softap_set_dns_addr gates the "advertise AP IP as DNS server" path on
 * this so DHCP clients never get pointed at a resolver that isn't there
 * (e.g. during boot, after a bind failure, or while the relay is
 * transitioning to/from enabled). */
bool dns_relay_is_healthy(void);

/* Override the upstream resolver (network byte order).
 * Pass 0 to fall back to the STA-learned DNS at forward time. */
void dns_relay_set_upstream(uint32_t ip_nbo);
uint32_t dns_relay_get_upstream(void);

/* Tell the relay which local IP to bind on. main.c calls this once
 * with the AP IP just after wifi_init_softap configures it, and again
 * whenever the AP IP changes (rare — operator-driven only). */
void dns_relay_set_bind_addr(uint32_t ip_nbo);

#ifdef __cplusplus
}
#endif
