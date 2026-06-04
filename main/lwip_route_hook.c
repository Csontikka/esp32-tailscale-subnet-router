/* Phase 1.5e exit-node default-route supervisor.
 *
 * 1.5c (Tailscale CGNAT routing for AP -> tailnet) works automatically:
 * the WG netif holds our 100.x.y.z/10 address, so lwIP's standard
 * subnet-match in ip4_route() sends any 100.x.x.x destination there.
 *
 * 1.5e (exit-node mode) needs the *default* route to point at the WG
 * netif. Internet traffic from AP clients then gets NAPT-ed into the
 * tunnel for the chosen exit node to forward out. The WG netif's own
 * encapsulated UDP packets must NOT use that default route (they would
 * loop back into the tunnel), so we pin the WG UDP socket to the
 * upstream STA netif via wireguardif_set_upstream_netif().
 *
 * A background task watches tailscale_exit_node_ip and the WG/STA
 * netif state and flips netif_default + the WG UDP pin in lockstep.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM stack */
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM */

#include "lwip_route_hook.h"
#include "microlink.h"
#include "tailscale_config.h"
#include "ping/ping_sock.h"

static const char *TAG = "exit_route";

/* Exit-node selection from tailscale_manager (0 = disabled). */
extern uint32_t tailscale_exit_node_ip;

/* Tailscale CGNAT: 100.64.0.0/10 */
#define TAILSCALE_CGNAT_NET   0x64400000UL
#define TAILSCALE_CGNAT_MASK  0xFFC00000UL

static inline bool ip_in_cgnat(uint32_t ip_host_order)
{
    return (ip_host_order & TAILSCALE_CGNAT_MASK) == TAILSCALE_CGNAT_NET;
}

static struct netif *find_wg_netif(void)
{
    extern struct netif *netif_list;
    for (struct netif *n = netif_list; n; n = n->next) {
        const ip4_addr_t *addr = netif_ip4_addr(n);
        if (addr == NULL || ip4_addr_isany_val(*addr)) continue;
        uint32_t ip = lwip_ntohl(ip4_addr_get_u32(addr));
        if (ip_in_cgnat(ip)) return n;
    }
    return NULL;
}

/* esp-netif names AP interfaces "ap" and STA interfaces "st". */
static inline bool netif_is_ap(const struct netif *n)
{
    return n && n->name[0] == 'a' && n->name[1] == 'p';
}

static inline bool netif_is_sta(const struct netif *n)
{
    return n && n->name[0] == 's' && n->name[1] == 't';
}

static struct netif *find_sta_netif(void)
{
    extern struct netif *netif_list;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (netif_is_sta(n) && netif_is_up(n) && netif_is_link_up(n)) {
            return n;
        }
    }
    return NULL;
}

static struct netif *s_original_default = NULL;
static bool s_wg_udp_pinned = false;
static esp_ping_handle_t s_keepalive_ping = NULL;
static uint32_t s_keepalive_target = 0;

static void keepalive_on_success(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }
static void keepalive_on_timeout(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }
static void keepalive_on_end(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }

static void keepalive_stop(void)
{
    if (s_keepalive_ping) {
        esp_ping_stop(s_keepalive_ping);
        esp_ping_delete_session(s_keepalive_ping);
        s_keepalive_ping = NULL;
        ESP_LOGI(TAG, "exit-node keepalive stopped");
    }
    s_keepalive_target = 0;
}

static void keepalive_start(uint32_t target_ip_hbo)
{
    if (s_keepalive_target == target_ip_hbo && s_keepalive_ping) {
        return;  /* already running for this target */
    }
    keepalive_stop();
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 25000;  /* matches tailscale's WG persistent keepalive */
    cfg.timeout_ms = 6000;
    cfg.data_size = 16;
    /* Same stack-sizing rationale as the exit_route supervisor: the
     * ESP-IDF "ping" task does socket I/O, ICMP packet building, and
     * fires ESP_LOG callbacks — 3072 B underran the vApplicationStack-
     * OverflowHook on a busier tunnel (multiple peers + accept-routes
     * + microlink probes contending). 5120 B gives >1 KB headroom on
     * the observed worst case. */
    cfg.task_stack_size = 5120;
    ip_addr_t addr;
    IP_SET_TYPE_VAL(addr, IPADDR_TYPE_V4);
    ip4_addr_set_u32(ip_2_ip4(&addr), lwip_htonl(target_ip_hbo));
    cfg.target_addr = addr;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = keepalive_on_success,
        .on_ping_timeout = keepalive_on_timeout,
        .on_ping_end = keepalive_on_end,
        .cb_args = NULL,
    };
    if (esp_ping_new_session(&cfg, &cbs, &s_keepalive_ping) == ESP_OK &&
        esp_ping_start(s_keepalive_ping) == ESP_OK) {
        s_keepalive_target = target_ip_hbo;
        ESP_LOGI(TAG, "exit-node keepalive started -> %lu.%lu.%lu.%lu (25s interval)",
                 (unsigned long)((target_ip_hbo >> 24) & 0xFF),
                 (unsigned long)((target_ip_hbo >> 16) & 0xFF),
                 (unsigned long)((target_ip_hbo >> 8) & 0xFF),
                 (unsigned long)(target_ip_hbo & 0xFF));
    } else {
        if (s_keepalive_ping) {
            esp_ping_delete_session(s_keepalive_ping);
            s_keepalive_ping = NULL;
        }
        ESP_LOGW(TAG, "exit-node keepalive failed to start");
    }
}

static void set_default_via_tcpip(struct netif *target)
{
    LOCK_TCPIP_CORE();
    netif_set_default(target);
    UNLOCK_TCPIP_CORE();
}

static void route_supervisor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "supervisor task started (exit_node=%lu)",
             (unsigned long)tailscale_exit_node_ip);
    while (1) {
        struct netif *wg = find_wg_netif();
        struct netif *sta = find_sta_netif();
        /* wireguardif only flips its netif to LINK_UP after at least one
         * peer has a valid session, which is exactly the chicken/egg case
         * we are trying to bootstrap for the exit node. Treat the netif
         * as usable as soon as it has its IP and is administratively up. */
        bool wg_ready = wg && netif_is_up(wg);

        if (s_original_default == NULL && netif_default != NULL &&
            (!wg || netif_default != wg)) {
            s_original_default = netif_default;
            ESP_LOGI(TAG, "Cached original default route: %c%c%d",
                     netif_default->name[0], netif_default->name[1],
                     netif_default->num);
        }

        struct netif *want = NULL;
        if (tailscale_exit_node_ip != 0) {
            if (wg_ready && sta != NULL) {
                if (!s_wg_udp_pinned) {
                    microlink_t *ml = tailscale_get_microlink();
                    esp_err_t pr = ml ? microlink_pin_wg_output_netif(ml, sta) : ESP_ERR_INVALID_STATE;
                    if (pr == ESP_OK) {
                        s_wg_udp_pinned = true;
                        ESP_LOGI(TAG, "WG UDP pinned to upstream %c%c%d",
                                 sta->name[0], sta->name[1], sta->num);
                    }
                }
                want = wg;
                /* Keep the WG session warm on the chosen exit node — without
                 * this its tailscaled lazy-trims us after ~4 minutes idle. */
                keepalive_start(tailscale_exit_node_ip);
            }
        } else {
            want = s_original_default;
            if (s_wg_udp_pinned) {
                microlink_t *ml = tailscale_get_microlink();
                if (ml) microlink_pin_wg_output_netif(ml, NULL);
                s_wg_udp_pinned = false;
                ESP_LOGI(TAG, "WG UDP unpinned (exit node off)");
            }
            keepalive_stop();
        }

        if (want != NULL && want != netif_default) {
            ESP_LOGI(TAG, "Switching default route: %c%c%d -> %c%c%d (exit_node=%lu)",
                     netif_default ? netif_default->name[0] : '?',
                     netif_default ? netif_default->name[1] : '?',
                     netif_default ? netif_default->num : 0,
                     want->name[0], want->name[1], want->num,
                     (unsigned long)tailscale_exit_node_ip);
            set_default_via_tcpip(want);
        }

        /* Phase 1.9n: rebuild accept-routes table from microlink peer info.
         * When tailscale_accept_routes=1 the route hook redirects any
         * destination matching one of these CIDRs into the WG tunnel — the
         * way `tailscale up --accept-routes` does on the official client. */
        if (tailscale_accept_routes) {
            microlink_t *ml = tailscale_get_microlink();
            int peer_n = ml ? microlink_get_peer_count(ml) : 0;
            int new_count = 0;
            for (int i = 0; i < peer_n &&
                 new_count < TAILSCALE_ACCEPTED_ROUTES_MAX; i++) {
                microlink_peer_info_t pi;
                if (microlink_get_peer_info(ml, i, &pi) != ESP_OK) continue;
                for (int r = 0; r < pi.subnet_route_count &&
                     new_count < TAILSCALE_ACCEPTED_ROUTES_MAX; r++) {
                    tailscale_accepted_routes[new_count].network =
                        pi.subnet_routes[r].network;
                    tailscale_accepted_routes[new_count].prefix_len =
                        pi.subnet_routes[r].prefix_len;
                    new_count++;
                }
            }
            tailscale_accepted_routes_count = new_count;
        } else if (tailscale_accepted_routes_count != 0) {
            tailscale_accepted_routes_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void lwip_route_hook_init(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    /* Stack budget: ESP_LOGI uses a 256 B vsnprintf scratch, the peer
     * iteration parks a ~150 B microlink_peer_info_t on the stack, and
     * set_default_via_tcpip() crosses into the lwIP TCPIP context. The
     * original 3072 B drove a vApplicationStackOverflowHook PANIC on a
     * busier tailnet (~6 peers + accept-routes). 5120 B leaves >1 KB
     * headroom on the observed worst case. */
    /* Stack in PSRAM (TCB stays internal): supervisor task, never deleted,
     * no SPI-flash writes; XIP keeps the cache live during flash ops. ~5K. */
    xTaskCreateWithCaps(route_supervisor_task, "exit_route", 5120, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
}

/* Linker-wrapped: ESP-IDF defines ip4_route_src_hook as a strong symbol
 * (components/lwip/port/hooks/lwip_default_hooks.c) and LWIP_HOOK_FILENAME
 * is reserved for IDF's own hook header. We can't replace the macro
 * definition cleanly, so we wrap the symbol at link time and call
 * __real_ip4_route_src_hook for anything we don't decide ourselves.
 *
 * Mirrors the upstream Tailscale Go decision (wgengine/userspace.go +
 * ipnlocal/local.go ExitNodeAllowLANAccess): rather than hard-coding
 * RFC 1918 ranges, walk the netif list and look for a *non-wg* netif
 * whose configured prefix contains the destination. Plus a pragmatic
 * RFC 1918 fallback for off-prefix LAN addresses (192.168.1.1 router
 * reachable via the STA gateway).
 *
 * Order of precedence:
 *   1. CGNAT dst (100.64.0.0/10) — always to wg netif (Phase 1.5c).
 *   2. Exit-node on AND lan_bypass on AND dst is in some non-wg netif's
 *      prefix → that netif.
 *   3. Exit-node on AND lan_bypass on AND dst is RFC1918 private → STA.
 *   4. Fall through to ESP-IDF default behavior (matching src IP →
 *      netif).
 */
extern struct netif *__real_ip4_route_src_hook(const ip4_addr_t *src,
                                                 const ip4_addr_t *dest);

/* Diagnostic log throttle for the route hook. The hook is on the hot
 * forwarding path (every IP packet routed through the ESP), so any
 * unconditional log would flood the UART. 100 ms cap = max 10 log
 * lines/sec across all decision branches. Per-branch granularity isn't
 * needed for the current upload-stall investigation — we only want to
 * see whether the AP→phone-upload SYN burst lands on the WG netif
 * (suspected) or STA (expected). */
static uint32_t s_route_log_last_ms = 0;

/* Per-packet route-hook tracing is OFF by default: on a busy tunnel it was
 * ~5 lines/s (the dominant share of the WARN volume on the SD recorder) and
 * it carries no control-plane-wedge signal — it only confirms a packet
 * routed, which the data plane already proves. Flip it back on at runtime
 * (no rebuild) with lwip_route_hook_set_verbose(true) for a routing debug
 * session; nothing is lost, the trace just stays quiet until asked for. */
static volatile bool s_route_log_enabled = false;

void lwip_route_hook_set_verbose(bool enabled)
{
    s_route_log_enabled = enabled;
}

static inline bool should_log_route_hook(void)
{
    if (!s_route_log_enabled) return false;
    uint32_t now = esp_log_timestamp();
    if (now - s_route_log_last_ms < 100) return false;
    s_route_log_last_ms = now;
    return true;
}

struct netif *__wrap_ip4_route_src_hook(const ip4_addr_t *src,
                                          const ip4_addr_t *dest)
{
    if (dest == NULL) return NULL;
    uint32_t dst_hbo = lwip_ntohl(dest->addr);
    uint32_t src_hbo = (src != NULL) ? lwip_ntohl(src->addr) : 0;

    /* 1. Tailnet CGNAT — always WG. */
    if (ip_in_cgnat(dst_hbo)) {
        if (should_log_route_hook()) {
            ESP_LOGW(TAG, "[ROUTE_HOOK] CGNAT src=%lu.%lu.%lu.%lu dst=%lu.%lu.%lu.%lu -> wg",
                     (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                     (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF);
        }
        return find_wg_netif();
    }

    /* 1b. Accept-routes — when enabled, peer-advertised subnet routes from
     * the netmap (AllowedIPs) are honoured: traffic to any matching CIDR
     * goes into the WG tunnel just like CGNAT-bound traffic does.
     *
     * IMPORTANT: only apply this to FORWARDED traffic (where src is some
     * AP-client IP, not one of our own netif IPs). The ESP itself sends
     * its own UDP probes — microlink DISCO pings out to peer endpoints
     * that may legitimately sit on 192.168.x.x LANs reachable via STA —
     * and those would loop into the tunnel if we redirected them. Local
     * origin keeps the IDF default behaviour: STA-bound, no WG. */
    if (tailscale_accept_routes && tailscale_accepted_routes_count > 0 &&
        src && !ip4_addr_isany_val(*src)) {
        bool src_is_local = false;
        extern struct netif *netif_list;
        for (struct netif *n = netif_list; n; n = n->next) {
            const ip4_addr_t *na = netif_ip4_addr(n);
            if (na && ip4_addr_cmp(src, na)) { src_is_local = true; break; }
        }
        if (!src_is_local) {
            for (int i = 0; i < tailscale_accepted_routes_count; i++) {
                uint8_t plen = tailscale_accepted_routes[i].prefix_len;
                if (plen == 0 || plen > 32) continue;
                uint32_t mask = (plen == 32) ? 0xFFFFFFFFUL
                                             : (0xFFFFFFFFUL << (32 - plen));
                if ((dst_hbo & mask) ==
                    (tailscale_accepted_routes[i].network & mask)) {
                    if (should_log_route_hook()) {
                        uint32_t net = tailscale_accepted_routes[i].network;
                        ESP_LOGW(TAG, "[ROUTE_HOOK] ACCEPT_ROUTE src=%lu.%lu.%lu.%lu "
                                 "dst=%lu.%lu.%lu.%lu match=%lu.%lu.%lu.%lu/%u -> wg",
                                 (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                                 (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF,
                                 (net>>24)&0xFF, (net>>16)&0xFF, (net>>8)&0xFF, net&0xFF, plen);
                    }
                    return find_wg_netif();
                }
            }
        }
    }

    /* 2. Exit-node + lan_bypass: send LAN-class traffic to STA, not the
     * tunnel. Two passes, mirroring tailscale Go but extended:
     *   2a. Netif-prefix match — the upstream behaviour. Catches the
     *       STA's DHCP-assigned subnet directly (e.g. 192.168.1.0/24).
     *   2b. RFC 1918 private fallback — sends non-netif-prefix private
     *       addresses (e.g. 192.168.1.1 on the home router behind the
     *       STA's gateway) to the STA. This is *broader* than upstream
     *       tailscale, which only knows about netif-attached prefixes,
     *       but matches user intent: "anything reachable through my
     *       physical LAN should not detour via the exit node". */
    if (tailscale_exit_node_ip != 0 && tailscale_lan_bypass) {
        extern struct netif *netif_list;
        struct netif *sta_match = NULL;

        for (struct netif *n = netif_list; n; n = n->next) {
            const ip4_addr_t *addr = netif_ip4_addr(n);
            const ip4_addr_t *mask = netif_ip4_netmask(n);
            if (addr == NULL || mask == NULL) continue;
            if (ip4_addr_isany_val(*addr)) continue;
            /* Skip the WG netif (CGNAT — handled in step 1, would loop). */
            if (ip_in_cgnat(lwip_ntohl(ip4_addr_get_u32(addr)))) continue;

            /* 2a: dst sits inside this netif's own prefix. This MUST include
             * the AP netif — packets to 192.168.4.x (AP clients, e.g. Pi at
             * 192.168.4.2 forwarded from a tailnet peer) need to egress via
             * AP. Otherwise the RFC1918 fallback below would catch
             * 192.168.4.0/16 and redirect the AP-bound traffic to STA — a
             * silent black-hole. */
            if ((dest->addr & mask->addr) == (addr->addr & mask->addr)) {
                if (should_log_route_hook()) {
                    ESP_LOGW(TAG, "[ROUTE_HOOK] LAN_SUBNET src=%lu.%lu.%lu.%lu "
                             "dst=%lu.%lu.%lu.%lu -> %c%c%u",
                             (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                             (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF,
                             n->name[0], n->name[1], n->num);
                }
                return n;
            }
            /* Remember the first non-wg, non-AP netif for the RFC1918
             * fallback below — that's where we'd route LAN traffic
             * that doesn't sit in our own subnet but is reachable
             * via the upstream router. */
            if (sta_match == NULL && netif_is_sta(n) &&
                netif_is_up(n) && netif_is_link_up(n)) {
                sta_match = n;
            }
        }

        /* 2b: RFC 1918 fallback. */
        bool is_private = ((dst_hbo & 0xFF000000UL) == 0x0A000000UL) ||  /* 10.0.0.0/8 */
                          ((dst_hbo & 0xFFF00000UL) == 0xAC100000UL) ||  /* 172.16.0.0/12 */
                          ((dst_hbo & 0xFFFF0000UL) == 0xC0A80000UL);    /* 192.168.0.0/16 */
        if (is_private && sta_match != NULL) {
            if (should_log_route_hook()) {
                ESP_LOGW(TAG, "[ROUTE_HOOK] LAN_RFC1918 src=%lu.%lu.%lu.%lu "
                         "dst=%lu.%lu.%lu.%lu -> %c%c%u",
                         (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                         (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF,
                         sta_match->name[0], sta_match->name[1], sta_match->num);
            }
            return sta_match;
        }
    }

    /* 3. Self-origin public destination (NEW 2026-05-20):
     * Raw sockets and TLS connect() calls that originate on the ESP
     * itself often pass src=NULL or src=INADDR_ANY into lwIP's route
     * lookup. In exit-node mode we WANT non-self traffic to follow the
     * tunnel, but the ESP's own DERP/STUN/control-plane sessions are
     * what KEEP the tunnel alive — routing those via wg would loop
     * (chicken & egg). So we split here:
     *   - self-origin to public dst → STA upstream
     *   - forwarded (non-self) to public dst → WG tunnel (the actual
     *     exit-node egress). This is the egress path we previously
     *     relied on netif_default for, but the esp_netif framework
     *     keeps resetting netif_default back to STA on link/DHCP
     *     events — so we must steer explicitly from the hook. */
    if (tailscale_exit_node_ip != 0) {
        bool src_is_self = (src == NULL) || ip4_addr_isany_val(*src);
        if (!src_is_self) {
            extern struct netif *netif_list;
            for (struct netif *n = netif_list; n; n = n->next) {
                const ip4_addr_t *na = netif_ip4_addr(n);
                if (na && ip4_addr_cmp(src, na)) { src_is_self = true; break; }
            }
        }
        bool is_priv = ((dst_hbo & 0xFF000000UL) == 0x0A000000UL) ||
                       ((dst_hbo & 0xFFF00000UL) == 0xAC100000UL) ||
                       ((dst_hbo & 0xFFFF0000UL) == 0xC0A80000UL);
        if (src_is_self) {
            /* CGNAT was already returned in step 1; here we just need
             * to bypass exit-node for the remaining public space. */
            if (!is_priv) {
                extern struct netif *netif_list;
                for (struct netif *n = netif_list; n; n = n->next) {
                    if (netif_is_sta(n) && netif_is_up(n) && netif_is_link_up(n)) {
                        if (should_log_route_hook()) {
                            ESP_LOGW(TAG, "[ROUTE_HOOK] SELF_PUBLIC src=%lu.%lu.%lu.%lu "
                                     "dst=%lu.%lu.%lu.%lu -> %c%c%u",
                                     (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                                     (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF,
                                     n->name[0], n->name[1], n->num);
                        }
                        return n;
                    }
                }
            }
        } else if (!is_priv) {
            /* Forwarded packet (AP client → public). Steer into the
             * tunnel explicitly — don't fall through to the IDF default
             * which would land on STA (because esp_netif keeps
             * netif_default pinned to STA across DHCP renews). */
            struct netif *wg = find_wg_netif();
            if (wg && netif_is_up(wg)) {
                if (should_log_route_hook()) {
                    ESP_LOGW(TAG, "[ROUTE_HOOK] FWD_PUBLIC src=%lu.%lu.%lu.%lu "
                             "dst=%lu.%lu.%lu.%lu -> wg",
                             (src_hbo>>24)&0xFF, (src_hbo>>16)&0xFF, (src_hbo>>8)&0xFF, src_hbo&0xFF,
                             (dst_hbo>>24)&0xFF, (dst_hbo>>16)&0xFF, (dst_hbo>>8)&0xFF, dst_hbo&0xFF);
                }
                return wg;
            }
        }
    }

    /* 4. Fall through to ESP-IDF default. The IDF default uses
     * ip4_addr_cmp(src, netif->ip) (EXACT equality, not prefix-match)
     * to find a source-binding netif. For forwarded AP-client packets
     * exit-node-off this lands on netif_default (= STA), which is the
     * desired vanilla behaviour. */
    return __real_ip4_route_src_hook(src, dest);
}

/* ---------------------------------------------------------------------
 * Route explanation — used by /diag "Route lookup". Runs the exact same
 * decision tree as __wrap_ip4_route_src_hook so the GUI shows the live
 * hook behaviour and won't drift from it. Caller passes a destination
 * in host byte order; we emit a netif label ("wg0", "st1", "ap2") and
 * a short reason string.
 * --------------------------------------------------------------------- */
extern int32_t tailscale_accept_routes;
extern int tailscale_accepted_routes_count;

static void name_netif(const struct netif *n, char *buf, size_t buf_size)
{
    if (!n) { snprintf(buf, buf_size, "no-route"); return; }
    snprintf(buf, buf_size, "%c%c%d", n->name[0], n->name[1], n->num);
}

/* Strip the trailing tailnet suffix (e.g. ".tailnet.ts.net") from a
 * peer hostname so the explanation reads "via home-server" rather than the
 * full FQDN. Falls back to the whole string when no dot is present. */
static void short_peer_name(const char *src, char *dst, size_t dst_size)
{
    if (!src || !*src) { snprintf(dst, dst_size, "(unnamed)"); return; }
    const char *dot = strchr(src, '.');
    size_t n = dot ? (size_t)(dot - src) : strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void route_explain(uint32_t src_hbo, uint32_t dst_hbo,
                   char *out_netif, size_t out_netif_size,
                   char *out, size_t out_size)
{
    extern struct netif *netif_list;
    extern struct netif *netif_default;

    out[0] = '\0';
    name_netif(NULL, out_netif, out_netif_size);

    ip4_addr_t dst;
    dst.addr = lwip_htonl(dst_hbo);

    /* Treat src_hbo==0 as "self-origin" (matches the hook's src==NULL /
     * src==INADDR_ANY case). Non-zero src is a forwarded packet. We also
     * flag a non-zero src that exactly matches one of our own netif IPs
     * as self — that's how the live hook decides between self vs forward. */
    bool src_is_self = (src_hbo == 0);
    if (!src_is_self) {
        for (struct netif *n = netif_list; n; n = n->next) {
            const ip4_addr_t *na = netif_ip4_addr(n);
            if (na && lwip_ntohl(na->addr) == src_hbo) { src_is_self = true; break; }
        }
    }

    /* 1. CGNAT. */
    if (ip_in_cgnat(dst_hbo)) {
        struct netif *wg = find_wg_netif();
        name_netif(wg, out_netif, out_netif_size);
        snprintf(out, out_size,
                 "CGNAT (100.64.0.0/10) → WG tunnel (Phase 1.5c built-in)");
        return;
    }

    /* 1b. Accept-routes table lookup — only kicks in for FORWARDED
     * traffic (mirrors the hook: peer subnet routes are programmed
     * only for AP-client packets, not for the ESP's own LAN probes). */
    if (!src_is_self &&
        tailscale_accept_routes && tailscale_accepted_routes_count > 0) {
        for (int i = 0; i < tailscale_accepted_routes_count; i++) {
            uint8_t plen = tailscale_accepted_routes[i].prefix_len;
            if (plen == 0 || plen > 32) continue;
            uint32_t mask = (plen == 32) ? 0xFFFFFFFFUL
                                         : (0xFFFFFFFFUL << (32 - plen));
            if ((dst_hbo & mask) ==
                (tailscale_accepted_routes[i].network & mask)) {
                struct netif *wg = find_wg_netif();
                name_netif(wg, out_netif, out_netif_size);
                uint32_t nw = tailscale_accepted_routes[i].network;
                /* Resolve the advertising peer's hostname. */
                char peer_host[40] = "unknown peer";
                microlink_t *ml = tailscale_get_microlink();
                if (ml) {
                    int pn = microlink_get_peer_count(ml);
                    for (int p = 0; p < pn; p++) {
                        microlink_peer_info_t pi;
                        if (microlink_get_peer_info(ml, p, &pi) != ESP_OK) continue;
                        for (int r = 0; r < pi.subnet_route_count; r++) {
                            if (pi.subnet_routes[r].network == nw &&
                                pi.subnet_routes[r].prefix_len == plen) {
                                short_peer_name(pi.hostname, peer_host,
                                                sizeof(peer_host));
                                goto found_peer;
                            }
                        }
                    }
                }
            found_peer: ;
                snprintf(out, out_size,
                         "matches accepted route %lu.%lu.%lu.%lu/%u "
                         "→ WG tunnel via %s",
                         (unsigned long)((nw >> 24) & 0xFF),
                         (unsigned long)((nw >> 16) & 0xFF),
                         (unsigned long)((nw >>  8) & 0xFF),
                         (unsigned long)( nw        & 0xFF),
                         plen, peer_host);
                return;
            }
        }
    }

    /* 2. Exit-node + lan-bypass — netif prefix match, then RFC1918. */
    if (tailscale_exit_node_ip != 0 && tailscale_lan_bypass) {
        struct netif *sta_match = NULL;
        for (struct netif *n = netif_list; n; n = n->next) {
            const ip4_addr_t *addr = netif_ip4_addr(n);
            const ip4_addr_t *mask = netif_ip4_netmask(n);
            if (!addr || !mask || ip4_addr_isany_val(*addr)) continue;
            if (ip_in_cgnat(lwip_ntohl(ip4_addr_get_u32(addr)))) continue;
            if ((dst.addr & mask->addr) == (addr->addr & mask->addr)) {
                name_netif(n, out_netif, out_netif_size);
                snprintf(out, out_size,
                         "netif prefix match (lan-bypass step 2a, exit-node on)");
                return;
            }
            if (!sta_match && netif_is_sta(n) && netif_is_up(n) && netif_is_link_up(n)) {
                sta_match = n;
            }
        }
        bool is_private = ((dst_hbo & 0xFF000000UL) == 0x0A000000UL) ||
                          ((dst_hbo & 0xFFF00000UL) == 0xAC100000UL) ||
                          ((dst_hbo & 0xFFFF0000UL) == 0xC0A80000UL);
        if (is_private && sta_match) {
            name_netif(sta_match, out_netif, out_netif_size);
            snprintf(out, out_size,
                     "RFC1918 destination, lan-bypass → STA upstream");
            return;
        }
    }

    /* 2c. Exit-node split — mirrors the live hook's step 3. Self-origin
     * public destinations egress via STA (chicken-and-egg bypass for the
     * ESP's own DERP/STUN sessions); forwarded public destinations egress
     * via the WG tunnel (the actual exit-node forwarding path). */
    if (tailscale_exit_node_ip != 0) {
        bool is_priv = ((dst_hbo & 0xFF000000UL) == 0x0A000000UL) ||
                       ((dst_hbo & 0xFFF00000UL) == 0xAC100000UL) ||
                       ((dst_hbo & 0xFFFF0000UL) == 0xC0A80000UL);
        if (!is_priv) {
            if (src_is_self) {
                for (struct netif *n = netif_list; n; n = n->next) {
                    if (netif_is_sta(n) && netif_is_up(n) && netif_is_link_up(n)) {
                        name_netif(n, out_netif, out_netif_size);
                        snprintf(out, out_size,
                                 "self-origin public dst → STA "
                                 "(bypassing exit-node to break chicken-and-egg)");
                        return;
                    }
                }
            } else {
                struct netif *wg = find_wg_netif();
                if (wg && netif_is_up(wg)) {
                    /* Look up the exit-node peer's hostname. */
                    char en_host[40] = "exit-node peer";
                    microlink_t *ml = tailscale_get_microlink();
                    if (ml) {
                        int pn = microlink_get_peer_count(ml);
                        for (int p = 0; p < pn; p++) {
                            microlink_peer_info_t pi;
                            if (microlink_get_peer_info(ml, p, &pi) != ESP_OK) continue;
                            if (pi.vpn_ip == tailscale_exit_node_ip) {
                                short_peer_name(pi.hostname, en_host, sizeof(en_host));
                                break;
                            }
                        }
                    }
                    name_netif(wg, out_netif, out_netif_size);
                    snprintf(out, out_size,
                             "forwarded public dst → WG tunnel via exit-node %s",
                             en_host);
                    return;
                }
            }
        }
    }

    /* 3. Fall through to subnet match + netif_default (mirrors what
     * ip4_route() does after our hook returns NULL). */
    for (struct netif *n = netif_list; n; n = n->next) {
        const ip4_addr_t *addr = netif_ip4_addr(n);
        const ip4_addr_t *mask = netif_ip4_netmask(n);
        if (!addr || !mask || ip4_addr_isany_val(*addr)) continue;
        if (!netif_is_up(n) || !netif_is_link_up(n)) continue;
        if ((dst.addr & mask->addr) == (addr->addr & mask->addr)) {
            name_netif(n, out_netif, out_netif_size);
            snprintf(out, out_size, "subnet match on %c%c%d's own prefix",
                     n->name[0], n->name[1], n->num);
            return;
        }
    }
    /* 4. netif_default (exit-node mode points it at wg0; otherwise STA). */
    if (netif_default) {
        name_netif(netif_default, out_netif, out_netif_size);
        if (tailscale_exit_node_ip != 0 &&
            netif_default == find_wg_netif()) {
            /* Look up the exit-node peer's hostname. */
            char en_host[40] = "exit-node peer";
            microlink_t *ml = tailscale_get_microlink();
            if (ml) {
                int pn = microlink_get_peer_count(ml);
                for (int p = 0; p < pn; p++) {
                    microlink_peer_info_t pi;
                    if (microlink_get_peer_info(ml, p, &pi) != ESP_OK) continue;
                    if (pi.vpn_ip == tailscale_exit_node_ip) {
                        short_peer_name(pi.hostname, en_host, sizeof(en_host));
                        break;
                    }
                }
            }
            snprintf(out, out_size,
                     "exit-node default route → WG tunnel via %s", en_host);
        } else {
            snprintf(out, out_size, "netif_default fallback");
        }
        return;
    }
    snprintf(out, out_size, "no route");
}

/* ---------------------------------------------------------------------
 * Phase 1.9k: ip_napt_forward wrapper.
 *
 * Hooked via --wrap=ip_napt_forward in main/CMakeLists.txt. We intercept
 * every NAPT decision made by ip4_forward and short-circuit it for any
 * packet destined to the Tailscale CGNAT range (100.64.0.0/10) — those
 * packets must keep their original source IP so the TCP 4-tuple at the
 * tailnet peer matches the connection the peer opened.
 *
 * Returning ERR_OK without touching the packet is exactly the
 * "no translation needed" path that ip4_forward expects: it then
 * proceeds with a vanilla forward to the chosen outbound netif (the
 * WG netif in our case), which the tailnet accepts because the
 * 192.168.4.0/24 subnet route is approved on the control plane and
 * the WG peer's allowed-source-IPs cover 0.0.0.0/0 in exit-node mode
 * (or specifically the subnet otherwise — the WG side is permissive
 * for inbound matches by destination).
 *
 * Non-CGNAT destinations get the original behavior (NAPT rewrites src
 * to the outbound netif IP) so the regular WAN path keeps working.
 *
 * Signature must match upstream:
 *   err_t ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr,
 *                         struct netif *inp, struct netif *outp);
 * --------------------------------------------------------------------- */
extern err_t __real_ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr,
                                      struct netif *inp, struct netif *outp);

/* Selective NAPT skip for tailnet (CGNAT) destinations.
 *
 * Why only TCP: TCP carries a strict 4-tuple match at the peer. When a
 * tailnet peer initiates an inbound connection to an AP-side server
 * (e.g. `peer → 192.168.4.2:80`), the Pi's SYN-ACK / data / ACK / FIN
 * packets MUST keep src=192.168.4.x — if NAPT rewrites them to
 * src=<wg netif CGNAT IP>, the peer drops them as not matching the
 * connection it opened. Pi-initiated TCP to tailnet peers also has to
 * preserve src because the peer's reply otherwise targets the wrong
 * address; it only works if the peer runs with --accept-routes (the
 * subnet route on the control plane is enabled).
 *
 * Why ICMP/UDP keep NAPT: those use the lwIP NAPT translation table
 * to undo the src rewrite end-to-end (ICMP id + UDP src-port), so the
 * Pi can ping/DNS-resolve toward tailnet peers EVEN if the peer is
 * NOT running --accept-routes — the peer just sees our WG IP. This is
 * the pre-1.9k behaviour we want to preserve for non-TCP flows.
 *
 * Net effect:
 *   inbound TCP (tailnet → AP):     works (peer 4-tuple OK)
 *   outbound TCP (AP → tailnet):    works only when peer has accept-routes
 *   inbound ICMP/UDP (tailnet → AP):works (no 4-tuple, NAPT-state aware)
 *   outbound ICMP/UDP (AP → tailnet):works via NAPT (src rewritten as before)
 */
err_t __wrap_ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr,
                               struct netif *inp, struct netif *outp)
{
    if (iphdr != NULL) {
        uint32_t dest_hbo = lwip_ntohl(iphdr->dest.addr);
        if (ip_in_cgnat(dest_hbo) && IPH_PROTO(iphdr) == 6 /* IPPROTO_TCP */) {
            return ERR_OK;
        }

        /* SNAT advertised subnet routes (opt-in, tailscale --snat-subnet-routes
         * semantics, default OFF). A packet forwarded FROM the tunnel (WG netif)
         * OUT through the STA uplink keeps its tailnet source (100.x) today,
         * because esp-lwip only masquerades when the INBOUND netif has napt set
         * (ip4_napt.c: `if (!inp->napt) return ERR_OK`) and only the AP netif is
         * napt-enabled. Without SNAT the uplink host replies to 100.x via its own
         * gateway, which has no tailnet route → return path lost. We opt this one
         * forward call into masquerade by transiently setting inp(WG)->napt around
         * __real, so the src is rewritten to the STA netif IP; the reply then
         * lands on the ESP and ip4_input's un-NAT (ip4.c, `!inp->napt && dest==inp`
         * on the STA side) reverses it automatically. Gated to inp==wg && outp==sta
         * so AP→tunnel / tunnel→AP / AP→WAN paths are untouched (no regression).
         * Runs in the single-threaded tcpip context, so the toggle is race-free. */
        if (tailscale_snat_subnet_routes && inp && outp && inp != outp &&
            !ip_in_cgnat(dest_hbo) &&
            inp == find_wg_netif() && netif_is_sta(outp)) {
            uint8_t saved = inp->napt;
            inp->napt = 1;
            err_t r = __real_ip_napt_forward(p, iphdr, inp, outp);
            inp->napt = saved;
            return r;
        }
    }
    return __real_ip_napt_forward(p, iphdr, inp, outp);
}
