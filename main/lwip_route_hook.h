/* Phase 1.5e exit-node default-route supervisor. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the background task that flips netif_default between the
 * upstream interface (STA) and the WireGuard netif depending on
 * `tailscale_exit_node_ip`. Idempotent; safe to call more than once. */
void lwip_route_hook_init(void);

/* Run a destination through the real hook decision tree and write a
 * human-readable explanation into `out`. Same code paths as the live
 * wrapper — if behaviour drifts here you'd see it on real packets too.
 *
 * dst_hbo: destination IPv4 in host byte order.
 * Returns the netif name (e.g. "wg0", "st1") written into out_netif,
 * or "no-route" if nothing matched.
 *
 * out / out_size: textual reason ("CGNAT match", "accept-routes via
 * nagyhaz", "STA subnet match", "lan-bypass RFC1918", "exit-node
 * default route", "netif_default fallback", etc.). */
#include <stddef.h>
#include <stdint.h>
void route_explain(uint32_t dst_hbo,
                   char *out_netif, size_t out_netif_size,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
