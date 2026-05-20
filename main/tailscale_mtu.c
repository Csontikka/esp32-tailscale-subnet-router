/* Tailscale tunnel MTU / MSS clamp / PMTU manager.
 *
 * See include/tailscale_mtu.h for the design rationale. This file owns:
 *   - NVS persistence for mode/fixed_mtu
 *   - Periodic re-evaluation (30s) so direct↔DERP transitions take effect
 *   - Application to the three knobs: ap_mss_clamp, ap_pmtu, wg netif mtu
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "lwip/netif.h"

#include "tailscale_config.h"     /* tailscale_enabled, tailscale_get_microlink */
#include "tailscale_mtu.h"
#include "microlink.h"
#include "nvs_params.h"

/* AP-side MSS clamp + ICMP frag-needed PMTU. tailscale_mtu computes
 * them; the AP-side netif hook (not yet migrated) will read them and
 * apply per packet. Defined here as the canonical owner. */
uint16_t ap_mss_clamp = 0;
uint16_t ap_pmtu      = 0;

static const char *TAG = "ts_mtu";

static ts_mtu_state_t s_state = {
    .mode      = TS_MTU_AUTO,
    .fixed_mtu = TS_MTU_DERP_DEFAULT,
    .eff_mtu   = 0,
    .eff_mss   = 0,
    .eff_pmtu  = 0,
    .source    = "off",
};

static esp_timer_handle_t s_poll_timer = NULL;

static struct netif *find_wg_netif(void)
{
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->name[0] == 'w' && n->name[1] == 'g') return n;
    }
    return NULL;
}

/* True if at least one tailnet peer is currently on a direct UDP path.
 * Falls back to false when microlink isn't running or the peer list is
 * empty — in either case we want the DERP-safe MTU floor. */
static bool any_direct_peer(void)
{
    struct microlink_s *ml = tailscale_get_microlink();
    if (!ml) return false;
    int n = microlink_get_peer_count(ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t pi;
        if (microlink_get_peer_info(ml, i, &pi) != ESP_OK) continue;
        if (pi.online && pi.direct_path) return true;
    }
    return false;
}

static void apply(uint16_t mtu, uint16_t mss, uint16_t pmtu, const char *src)
{
    s_state.eff_mtu  = mtu;
    s_state.eff_mss  = mss;
    s_state.eff_pmtu = pmtu;
    s_state.source   = src;

    ap_mss_clamp = mss;
    ap_pmtu      = pmtu;

    struct netif *wg = find_wg_netif();
    if (wg && mtu > 0 && wg->mtu != mtu) {
        ESP_LOGI(TAG, "wg netif MTU %u -> %u (%s)", wg->mtu, mtu, src);
        wg->mtu = mtu;
    }
}

void tailscale_mtu_update(void)
{
    /* Tunnel down: clear everything. Same behaviour as before the modu-
     * le existed in non-exit-node mode. */
    if (!tailscale_enabled) {
        apply(0, 0, 0, "off");
        return;
    }

    uint16_t mtu;
    const char *src;
    if (s_state.mode == TS_MTU_FIXED) {
        mtu = s_state.fixed_mtu;
        if (mtu < TS_MTU_MIN) mtu = TS_MTU_MIN;
        if (mtu > TS_MTU_MAX) mtu = TS_MTU_MAX;
        src = "user";
    } else {
        if (any_direct_peer()) {
            mtu = TS_MTU_DIRECT_DEFAULT;
            src = "auto-direct";
        } else {
            mtu = TS_MTU_DERP_DEFAULT;
            src = "auto-DERP";
        }
    }

    /* TCP MSS clamp = MTU - 20 (IP header) - 20 (TCP header). PMTU value
     * passed to the ICMP "frag needed" path is the IP MTU itself plus
     * 20 to account for the inner IP header; matches the pre-module
     * exit-node defaults (1380 / 1440 when MTU was 1420). */
    uint16_t mss  = (mtu > 40)  ? (mtu - 40)  : 0;
    uint16_t pmtu = (mtu > 0)   ? (mtu + 20)  : 0;

    apply(mtu, mss, pmtu, src);
}

esp_err_t tailscale_mtu_set(ts_mtu_mode_t mode, uint16_t fixed_mtu)
{
    if (mode == TS_MTU_FIXED) {
        if (fixed_mtu < TS_MTU_MIN || fixed_mtu > TS_MTU_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    s_state.mode      = mode;
    s_state.fixed_mtu = fixed_mtu;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8 (h, "ts_mtu_mode", (uint8_t)mode);
        nvs_set_u16(h, "ts_mtu",      fixed_mtu);
        nvs_commit(h);
        nvs_close(h);
    }
    tailscale_mtu_update();
    return ESP_OK;
}

ts_mtu_state_t tailscale_mtu_get(void)
{
    return s_state;
}

static void poll_timer_cb(void *arg)
{
    (void)arg;
    /* Re-evaluate the direct↔DERP picture. Cheap (one peer-list scan)
     * and idempotent — apply() only writes wg->mtu when it differs. */
    tailscale_mtu_update();
}

void tailscale_mtu_init(void)
{
    /* Load persisted settings. Defaults already in s_state initializer. */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t  m  = 0;
        uint16_t fm = 0;
        if (nvs_get_u8(h, "ts_mtu_mode", &m) == ESP_OK) {
            s_state.mode = (m == TS_MTU_FIXED) ? TS_MTU_FIXED : TS_MTU_AUTO;
        }
        if (nvs_get_u16(h, "ts_mtu", &fm) == ESP_OK && fm >= TS_MTU_MIN && fm <= TS_MTU_MAX) {
            s_state.fixed_mtu = fm;
        }
        nvs_close(h);
    }

    tailscale_mtu_update();

    /* 30 s polling — picks up direct/DERP transitions without needing
     * microlink callbacks. Cheap; no allocation per tick. */
    if (!s_poll_timer) {
        const esp_timer_create_args_t args = {
            .callback = poll_timer_cb,
            .name     = "ts_mtu_poll",
        };
        if (esp_timer_create(&args, &s_poll_timer) == ESP_OK) {
            esp_timer_start_periodic(s_poll_timer, 30ULL * 1000 * 1000);
        }
    }

    ESP_LOGI(TAG, "init: mode=%s fixed_mtu=%u eff=%u/%u/%u src=%s",
             s_state.mode == TS_MTU_FIXED ? "fixed" : "auto",
             s_state.fixed_mtu,
             s_state.eff_mtu, s_state.eff_mss, s_state.eff_pmtu,
             s_state.source);
}
