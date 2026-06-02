/* lwIP netif hook installer — ACL packet filter.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"

#include <string.h>
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/tcp.h"

#include "acl.h"
#include "netif_hooks.h"

/* MTU-management knobs published by tailscale_mtu.c. ap_mss_clamp is
 * the cap we enforce on every TCP SYN crossing the AP interface (so
 * neither side proposes a segment bigger than the tunnel can carry).
 * ap_pmtu is the next-hop MTU we report back when a DF=1 packet
 * arrives oversized — implements RFC 1191 Path MTU Discovery from
 * the AP side. 0 = feature disabled. */
extern uint16_t ap_mss_clamp;
extern uint16_t ap_pmtu;

static const char *TAG = "netif_hooks";

/* Original lwIP function pointers — captured before we install our
 * shim and called from inside it so packets that pass the ACL still
 * reach the rest of the stack unmodified. */
static netif_input_fn       original_sta_input       = NULL;
static netif_linkoutput_fn  original_sta_linkoutput  = NULL;
static netif_input_fn       original_ap_input        = NULL;
static netif_linkoutput_fn  original_ap_linkoutput   = NULL;

/* Wire-byte counters. Updated on the LWIP TCPIP task, read from the
 * HTTP server task. Display-only — torn reads at the 32-bit boundary
 * are tolerable for a UI gauge. */
static volatile uint64_t s_sta_bytes_in  = 0;
static volatile uint64_t s_sta_bytes_out = 0;
static volatile uint64_t s_ap_bytes_in   = 0;
static volatile uint64_t s_ap_bytes_out  = 0;

uint64_t netif_hooks_get_sta_bytes_in (void) { return s_sta_bytes_in;  }
uint64_t netif_hooks_get_sta_bytes_out(void) { return s_sta_bytes_out; }
uint64_t netif_hooks_get_ap_bytes_in  (void) { return s_ap_bytes_in;   }
uint64_t netif_hooks_get_ap_bytes_out (void) { return s_ap_bytes_out;  }

/* TTL override (0 = pass through). Read on every STA-out packet, set
 * by the /api/network POST handler + the boot-time NVS load. Single
 * byte → atomic on Xtensa, no lock needed. */
static volatile uint8_t s_sta_ttl_override = 0;
void    netif_hooks_set_sta_ttl(uint8_t ttl) { s_sta_ttl_override = ttl; }
uint8_t netif_hooks_get_sta_ttl(void)        { return s_sta_ttl_override; }

/* Clamp the TCP MSS option in a SYN packet to max_mss. Operates on raw
 * Ethernet frames (14-byte header + IPv4 + TCP). Updates the TCP
 * checksum incrementally per RFC 1624. Pbuf must carry the whole TCP
 * header + options in its first segment; lwIP allocates those
 * contiguously for incoming packets so this is always the case in
 * practice. No-op on every other shape (UDP, IPv6, runts, non-SYN). */
static void clamp_tcp_mss(struct pbuf *p, uint16_t max_mss)
{
    if (max_mss == 0 || p == NULL) return;
    if (p->len < 14 + 20 + 20) return;
    uint8_t *payload = (uint8_t *)p->payload;
    if (payload[12] != 0x08 || payload[13] != 0x00) return;        /* IPv4? */
    struct ip_hdr *iphdr = (struct ip_hdr *)(payload + 14);
    if (IPH_V(iphdr) != 4 || IPH_PROTO(iphdr) != IP_PROTO_TCP) return;
    uint16_t ip_hdr_len = IPH_HL(iphdr) * 4;
    if (p->len < 14 + ip_hdr_len + 20) return;
    uint8_t *tcphdr = payload + 14 + ip_hdr_len;
    /* SYN bit: TCP flags byte at offset 13, SYN = bit 1. */
    if (!(tcphdr[13] & 0x02)) return;
    uint8_t tcp_hdr_len = (tcphdr[12] >> 4) * 4;
    if (tcp_hdr_len < 20 || p->len < 14 + ip_hdr_len + tcp_hdr_len) return;

    /* Walk the TCP options TLV. MSS option is kind=2, len=4, payload
     * is 16-bit MSS in network order. */
    uint8_t *opt     = tcphdr + 20;
    uint8_t *opt_end = tcphdr + tcp_hdr_len;
    while (opt < opt_end) {
        uint8_t kind = opt[0];
        if (kind == 0) break;                  /* end-of-list */
        if (kind == 1) { opt++; continue; }    /* NOP */
        if (opt + 1 >= opt_end) break;
        uint8_t opt_len = opt[1];
        if (opt_len < 2 || opt + opt_len > opt_end) break;
        if (kind == 2 && opt_len == 4) {
            uint16_t *mss_ptr = (uint16_t *)(opt + 2);
            if (lwip_ntohs(*mss_ptr) > max_mss) {
                uint16_t old_mss_net = *mss_ptr;
                *mss_ptr = lwip_htons(max_mss);
                /* RFC 1624 incremental: HC' = ~(~HC + ~m + m'). */
                uint16_t *chksum = (uint16_t *)(tcphdr + 16);
                uint32_t sum = (uint16_t)~(*chksum);
                sum += (uint16_t)~old_mss_net;
                sum += lwip_htons(max_mss);
                while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
                *chksum = (uint16_t)~sum;
            }
            break;
        }
        opt += opt_len;
    }
}

/* Send an ICMP "Fragmentation Needed and DF set" (RFC 1191 type 3 /
 * code 4) back to an AP client. Triggered when the client sends a
 * DF=1 frame bigger than `mtu`. Source IP comes from the netif so
 * we don't need a separate cached global. */
static void send_icmp_frag_needed(struct pbuf *p, struct netif *netif, uint16_t mtu)
{
    if (p == NULL || netif == NULL || original_ap_linkoutput == NULL) return;
    if (p->len < 14 + 20) return;
    uint8_t *eth = (uint8_t *)p->payload;
    if (eth[12] != 0x08 || eth[13] != 0x00) return;                /* IPv4 only */
    struct ip_hdr *orig_ip = (struct ip_hdr *)(eth + 14);
    if (IPH_V(orig_ip) != 4) return;
    uint16_t ip_total_len = lwip_ntohs(IPH_LEN(orig_ip));
    if (ip_total_len <= mtu) return;

    uint16_t flags_offset = lwip_ntohs(IPH_OFFSET(orig_ip));
    if (!(flags_offset & IP_DF)) return;                           /* DF not set */
    if (flags_offset & IP_OFFMASK) return;                         /* later fragment */
    if (IPH_PROTO(orig_ip) == IP_PROTO_ICMP) return;               /* no feedback loops */

    uint16_t orig_ihl = IPH_HL(orig_ip) * 4;
    if (orig_ihl < 20 || orig_ihl > 60) return;

    /* RFC 1191 reply body: 4 bytes (unused16 + nexthop-mtu16) + the
     * triggering IP header + first 8 bytes of payload. */
    uint16_t avail_data = (p->len > 14u + orig_ihl) ? (p->len - 14u - orig_ihl) : 0;
    if (avail_data > 8) avail_data = 8;
    uint16_t icmp_body  = 4 + orig_ihl + avail_data;
    uint16_t icmp_total = 4 + icmp_body;
    uint16_t new_ip_len = 20 + icmp_total;
    uint16_t frame_len  = 14 + new_ip_len;

    struct pbuf *resp = pbuf_alloc(PBUF_RAW, frame_len, PBUF_RAM);
    if (!resp) return;
    uint8_t *buf = (uint8_t *)resp->payload;
    memset(buf, 0, frame_len);

    /* Ethernet — swap MACs so the reply heads back to the client. */
    memcpy(buf + 0, eth + 6, 6);
    memcpy(buf + 6, eth + 0, 6);
    buf[12] = 0x08; buf[13] = 0x00;

    /* IPv4 header. */
    struct ip_hdr *rip = (struct ip_hdr *)(buf + 14);
    IPH_VHL_SET(rip, 4, 5);
    IPH_TOS_SET(rip, 0);
    IPH_LEN_SET(rip, lwip_htons(new_ip_len));
    IPH_ID_SET(rip, 0);
    IPH_OFFSET_SET(rip, 0);
    IPH_TTL_SET(rip, 64);
    IPH_PROTO_SET(rip, IP_PROTO_ICMP);
    IPH_CHKSUM_SET(rip, 0);
    rip->src.addr  = netif->ip_addr.u_addr.ip4.addr;
    rip->dest.addr = orig_ip->src.addr;
    IPH_CHKSUM_SET(rip, inet_chksum(rip, 20));

    /* ICMP destination-unreachable / fragmentation-needed. */
    uint8_t *icmp = buf + 14 + 20;
    icmp[0] = 3;                       /* type: destination unreachable */
    icmp[1] = 4;                       /* code: frag-needed, DF set     */
    icmp[2] = 0; icmp[3] = 0;          /* checksum (filled below)        */
    icmp[4] = 0; icmp[5] = 0;          /* reserved (RFC 1191 unused)     */
    icmp[6] = (uint8_t)(mtu >> 8);
    icmp[7] = (uint8_t)(mtu & 0xff);
    memcpy(icmp + 8, orig_ip, orig_ihl);
    if (avail_data > 0) {
        memcpy(icmp + 8 + orig_ihl, (uint8_t *)orig_ip + orig_ihl, avail_data);
    }
    *((uint16_t *)(icmp + 2)) = inet_chksum(icmp, icmp_total);

    original_ap_linkoutput(netif, resp);
    pbuf_free(resp);
}

/* Rewrite the IPv4 TTL field of an outgoing STA frame in place and patch
 * the header checksum (RFC 1624 incremental update). Pbuf must be at
 * least eth-header + ip-header big and carry IPv4. No-op on every other
 * shape — IPv6, ARP, runt frames pass through untouched. */
static inline void apply_sta_ttl_override(struct pbuf *p)
{
    uint8_t override = s_sta_ttl_override;
    if (override == 0 || !p) return;
    if (p->len < 14 + (int)sizeof(struct ip_hdr)) return;
    uint8_t *payload = (uint8_t *)p->payload;
    /* Ethertype IPv4? */
    if (payload[12] != 0x08 || payload[13] != 0x00) return;
    struct ip_hdr *iphdr = (struct ip_hdr *)(payload + 14);
    if (IPH_V(iphdr) != 4) return;
    if (iphdr->_ttl == override) return; /* already at target — skip checksum recompute */

    /* RFC 1624: HC' = ~(~HC + ~m + m'), where m and m' span the same
     * 16-bit word as the TTL byte. _ttl is the high byte of that word
     * (protocol field is the low byte), so we read the word, rewrite
     * the high byte, and incrementally update the checksum. */
    uint16_t old_word = *(uint16_t *)&iphdr->_ttl;
    iphdr->_ttl = override;
    uint16_t new_word = *(uint16_t *)&iphdr->_ttl;
    uint32_t sum = (uint16_t)~iphdr->_chksum;
    sum += (uint16_t)~old_word;
    sum += new_word;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    iphdr->_chksum = (uint16_t)~sum;
}

/* ACL check. (A PCAP tap used to live here; the capture feature was
 * removed.) Thin wrapper so the hook call sites stay unchanged. */
static inline uint8_t acl_check_and_tap(uint8_t list_id, struct pbuf *p, bool is_ap)
{
    (void)is_ap;
    return acl_check_packet(list_id, p);
}

static inline bool acl_drops(uint8_t action)
{
    return (action != ACL_NO_MATCH) && ((action & 0x01) == ACL_DENY);
}

static err_t sta_input_hook(struct pbuf *p, struct netif *netif)
{
    if (p) s_sta_bytes_in += p->tot_len;
    if (acl_drops(acl_check_and_tap(ACL_TO_ESP, p, false))) { pbuf_free(p); return ERR_OK; }
    return original_sta_input ? original_sta_input(p, netif) : ERR_VAL;
}

static err_t sta_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
    if (p) s_sta_bytes_out += p->tot_len;
    /* TTL rewrite happens BEFORE the ACL check so the ACL rules see
     * the value that actually goes out the wire. */
    apply_sta_ttl_override(p);
    if (acl_drops(acl_check_and_tap(ACL_FROM_ESP, p, false))) { return ERR_OK; }
    return original_sta_linkoutput ? original_sta_linkoutput(netif, p) : ERR_VAL;
}

static err_t ap_input_hook(struct pbuf *p, struct netif *netif)
{
    if (p) s_ap_bytes_in += p->tot_len;
    if (acl_drops(acl_check_and_tap(ACL_TO_AP, p, true))) { pbuf_free(p); return ERR_OK; }
    /* PMTU: tell the client to back off when it sends a DF packet
     * bigger than the tunnel can carry. Runs BEFORE we forward so the
     * client gets the signal even when we'd otherwise drop the frame. */
    if (ap_pmtu > 0) send_icmp_frag_needed(p, netif, ap_pmtu);
    /* MSS-clamp on TCP SYNs from the client side so neither end of the
     * connection proposes a segment bigger than the tunnel allows. */
    clamp_tcp_mss(p, ap_mss_clamp);
    return original_ap_input ? original_ap_input(p, netif) : ERR_VAL;
}

static err_t ap_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
    if (p) s_ap_bytes_out += p->tot_len;
    if (acl_drops(acl_check_and_tap(ACL_FROM_AP, p, true))) { return ERR_OK; }
    /* And clamp again on outbound — SYN-ACK from upstream toward the
     * client also needs its MSS bounded. */
    clamp_tcp_mss(p, ap_mss_clamp);
    return original_ap_linkoutput ? original_ap_linkoutput(netif, p) : ERR_VAL;
}

void netif_hooks_init(void)
{
    static bool installed = false;
    if (installed) return;

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (sta) {
        struct netif *nif = esp_netif_get_netif_impl(sta);
        if (nif) {
            original_sta_input      = nif->input;
            original_sta_linkoutput = nif->linkoutput;
            nif->input              = sta_input_hook;
            nif->linkoutput         = sta_linkoutput_hook;
            ESP_LOGI(TAG, "STA hooks installed on %c%c%d", nif->name[0], nif->name[1], nif->num);
        }
    }
    if (ap) {
        struct netif *nif = esp_netif_get_netif_impl(ap);
        if (nif) {
            original_ap_input       = nif->input;
            original_ap_linkoutput  = nif->linkoutput;
            nif->input              = ap_input_hook;
            nif->linkoutput         = ap_linkoutput_hook;
            ESP_LOGI(TAG, "AP hooks installed on %c%c%d", nif->name[0], nif->name[1], nif->num);
        }
    }

    installed = (sta != NULL) || (ap != NULL);
}
