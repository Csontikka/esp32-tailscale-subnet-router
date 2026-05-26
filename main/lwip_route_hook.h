/* Phase 1.5e exit-node default-route supervisor. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the background task that flips netif_default between the
 * upstream interface (STA) and the WireGuard netif depending on
 * `tailscale_exit_node_ip`. Idempotent; safe to call more than once. */
void lwip_route_hook_init(void);

/* Enable/disable the per-packet [ROUTE_HOOK] WARN trace at runtime. OFF at
 * boot — it's high-volume and carries no wedge signal. Turn it on only for a
 * routing debug session; throttled to ~10 lines/s when enabled. */
void lwip_route_hook_set_verbose(bool enabled);

/* Run a destination through the real hook decision tree and write a
 * human-readable explanation into `out`. Same code paths as the live
 * wrapper — if behaviour drifts here you'd see it on real packets too.
 *
 * dst_hbo: destination IPv4 in host byte order.
 * Returns the netif name (e.g. "wg0", "st1") written into out_netif,
 * or "no-route" if nothing matched.
 *
 * out / out_size: textual reason ("CGNAT match", "accept-routes via
 * home-server", "STA subnet match", "lan-bypass RFC1918", "exit-node
 * default route", "netif_default fallback", etc.). */
#include <stddef.h>
#include <stdint.h>
/* src_hbo: simulated source IPv4 in host byte order.
 *   0 → "self-origin" (a socket on the ESP itself, src=any). This is
 *       what the Tools tab traceroute does in practice.
 *   non-zero → "forwarded packet" with the given source IP. Used to
 *       inspect what would happen to a packet from an AP client (e.g.
 *       the Pi at 192.168.31.5). The hook's full decision tree —
 *       including the forwarded-WG-on-exit-node branch — is replayed
 *       against this src. */
void route_explain(uint32_t src_hbo, uint32_t dst_hbo,
                   char *out_netif, size_t out_netif_size,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
