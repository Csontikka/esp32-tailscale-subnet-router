/* DNS-over-HTTP endpoint used by Tailscale exit-node clients.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once


#ifdef __cplusplus
extern "C" {
#endif

/* Start the exit-node DNS service on TAILSCALE_PEERAPI_PORT. Does nothing
 * unless the router offers itself as an exit node; call once at boot. */
void peer_dns_start(void);

#ifdef __cplusplus
}
#endif
