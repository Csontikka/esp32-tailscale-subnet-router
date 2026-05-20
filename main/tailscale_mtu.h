/* Tailscale tunnel MTU / MSS clamp / PMTU management.
 *
 * Owns the wg0 netif MTU and the AP-side TCP MSS / ICMP PMTU values
 * used by netif_hooks.c. Two modes:
 *
 *   AUTO  — picks 1420 when at least one peer is on a direct UDP path,
 *           1280 when all traffic is DERP-relayed (DERP adds ~60B of
 *           HTTP framing on top of WG, so 1280 is the safe floor).
 *   FIXED — user-supplied constant in [576..1500].
 *
 * tailscale_mtu_update() recomputes everything from the current
 * tailscale_enabled / microlink state and applies it. Safe to call from
 * any task; the actual netif->mtu write is gated by the wg netif being
 * present.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TS_MTU_AUTO  = 0,
    TS_MTU_FIXED = 1,
} ts_mtu_mode_t;

/* Sensible defaults — see header comment for the rationale. */
#define TS_MTU_DIRECT_DEFAULT  1420   /* matches WIREGUARDIF_MTU */
#define TS_MTU_DERP_DEFAULT    1280   /* DERP-safe floor */
#define TS_MTU_MIN              576   /* RFC 791 minimum reassembly */
#define TS_MTU_MAX             1500   /* Ethernet payload max */

typedef struct {
    /* Persisted (NVS): */
    ts_mtu_mode_t mode;
    uint16_t      fixed_mtu;   /* only meaningful when mode == TS_MTU_FIXED */
    /* Computed at every tailscale_mtu_update() call: */
    uint16_t      eff_mtu;     /* wg0 netif->mtu */
    uint16_t      eff_mss;     /* TCP SYN clamp value */
    uint16_t      eff_pmtu;    /* ICMP frag-needed PMTU */
    const char   *source;      /* "off" | "auto-direct" | "auto-DERP" | "user" */
} ts_mtu_state_t;

/* Loads NVS and runs the first update. Call once at boot, after
 * tailscale_init() and before lwip_route_hook_init(). */
void tailscale_mtu_init(void);

/* Recomputes eff_* from current tailscale_enabled / microlink state and
 * applies to ap_mss_clamp / ap_pmtu / wg netif->mtu. Idempotent. */
void tailscale_mtu_update(void);

/* Persists mode + fixed_mtu to NVS and runs an update. Returns ESP_OK on
 * success or ESP_ERR_INVALID_ARG on out-of-range fixed_mtu. */
esp_err_t tailscale_mtu_set(ts_mtu_mode_t mode, uint16_t fixed_mtu);

/* Read-only snapshot for UI / diagnostics. Returned struct is a copy. */
ts_mtu_state_t tailscale_mtu_get(void);

#ifdef __cplusplus
}
#endif
