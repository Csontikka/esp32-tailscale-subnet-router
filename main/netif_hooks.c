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

#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"

#include "acl.h"
#include "netif_hooks.h"
#include "pcap_capture.h"

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

/* ACL check + PCAP feed in one pass. Returns the raw acl_check_packet
 * result so the caller can act on the deny/allow bit; we side-effect
 * pcap_capture_packet here when either the ACL rule asked for it
 * (ACL_MONITOR) or the current pcap mode is promiscuous. */
static inline uint8_t acl_check_and_tap(uint8_t list_id, struct pbuf *p, bool is_ap)
{
    uint8_t action = acl_check_packet(list_id, p);
    bool monitored = (action != ACL_NO_MATCH) && (action & ACL_MONITOR);
    if (pcap_should_capture(monitored, is_ap)) pcap_capture_packet(p);
    return action;
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
    /* TTL rewrite happens BEFORE the ACL tap so the PCAP capture and
     * ACL rules both see the value that actually goes out the wire. */
    apply_sta_ttl_override(p);
    if (acl_drops(acl_check_and_tap(ACL_FROM_ESP, p, false))) { return ERR_OK; }
    return original_sta_linkoutput ? original_sta_linkoutput(netif, p) : ERR_VAL;
}

static err_t ap_input_hook(struct pbuf *p, struct netif *netif)
{
    if (p) s_ap_bytes_in += p->tot_len;
    if (acl_drops(acl_check_and_tap(ACL_TO_AP, p, true))) { pbuf_free(p); return ERR_OK; }
    return original_ap_input ? original_ap_input(p, netif) : ERR_VAL;
}

static err_t ap_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
    if (p) s_ap_bytes_out += p->tot_len;
    if (acl_drops(acl_check_and_tap(ACL_FROM_AP, p, true))) { return ERR_OK; }
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
