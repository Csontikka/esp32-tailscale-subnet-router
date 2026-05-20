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
    if (acl_drops(acl_check_and_tap(ACL_TO_ESP, p, false))) { pbuf_free(p); return ERR_OK; }
    return original_sta_input ? original_sta_input(p, netif) : ERR_VAL;
}

static err_t sta_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
    if (acl_drops(acl_check_and_tap(ACL_FROM_ESP, p, false))) { return ERR_OK; }
    return original_sta_linkoutput ? original_sta_linkoutput(netif, p) : ERR_VAL;
}

static err_t ap_input_hook(struct pbuf *p, struct netif *netif)
{
    if (acl_drops(acl_check_and_tap(ACL_TO_AP, p, true))) { pbuf_free(p); return ERR_OK; }
    return original_ap_input ? original_ap_input(p, netif) : ERR_VAL;
}

static err_t ap_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
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
