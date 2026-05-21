/* See dns_relay.h. Single forwarder task; per-query ephemeral upstream
 * socket. Stateless in the relay itself — all per-query state lives on
 * the task stack while it's waiting for the upstream reply. */

#include "dns_relay.h"

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DNS_RELAY_PORT          53
#define DNS_RELAY_BUF_SIZE      1024     /* RFC 1035 §2.3.4: 512 udp, but EDNS0 lifts it; 1024 is a safe ceiling. */
#define DNS_RELAY_UP_TIMEOUT_MS 3500
#define DNS_RELAY_TASK_STACK    6144     /* recvfrom + lwIP socket + ESP_LOG overhead; 4 KB was tight. */
#define DNS_RELAY_TASK_PRIO     4
#define DNS_RELAY_BOOT_DELAY_MS 12000    /* Settle window before we bind 53/udp — gives WiFi+netif+microlink time to come up cleanly. */

#define DNS_RELAY_NVS_NS        "tsr"
#define KEY_ENABLED             "dns_relay_en"
#define KEY_UPSTREAM            "dns_relay_up"

#define DEFAULT_FALLBACK_UPSTREAM 0x01010101UL  /* 1.1.1.1, network order written below. */

static const char *TAG = "dns_relay";

static volatile bool     s_enabled       = false;
static volatile uint32_t s_upstream_nbo  = 0;      /* 0 = use STA-learned */
static volatile uint32_t s_bind_nbo      = 0;      /* AP IP; 0 = wait, don't bind yet */
static volatile bool     s_socket_dirty  = false;  /* tell the task to rebind */
static volatile bool     s_healthy       = false;  /* the task is bound and serving — softap_set_dns_addr checks this before advertising AP IP as DNS */

static TaskHandle_t s_task = NULL;

/* Weak hook fired on every false→true health transition. main.c
 * overrides this to (re-)run softap_set_dns_addr so the DHCP-advertised
 * resolver flips to the AP IP the instant the relay is actually ready.
 * Weak so unit tests / future callers without a main.c override link. */
__attribute__((weak)) void dns_relay_on_healthy(void) {}

/* Resolve the live upstream IP every forward. Priority:
 *   1. Explicit override (s_upstream_nbo)
 *   2. STA-learned DNS
 *   3. 1.1.1.1 fallback (so we never serve garbage). */
static uint32_t pick_upstream(void)
{
    if (s_upstream_nbo) return s_upstream_nbo;

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_dns_info_t info = {0};
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &info) == ESP_OK) {
            uint32_t a = info.ip.u_addr.ip4.addr;
            if (a != 0) return a;
        }
    }
    /* 1.1.1.1 in network byte order. */
    return PP_HTONL(DEFAULT_FALLBACK_UPSTREAM);
}

/* Open + bind the listening socket. Returns the fd or -1. */
static int open_listen_sock(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "listen socket() failed: errno=%d", errno);
        return -1;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_RELAY_PORT),
        .sin_addr.s_addr = s_bind_nbo ? s_bind_nbo : INADDR_ANY,
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(s);
        return -1;
    }
    struct timeval rcvto = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
    ESP_LOGI(TAG, "listening on %d.%d.%d.%d:%d",
             (int)(addr.sin_addr.s_addr        & 0xFF),
             (int)((addr.sin_addr.s_addr >> 8) & 0xFF),
             (int)((addr.sin_addr.s_addr >> 16)& 0xFF),
             (int)((addr.sin_addr.s_addr >> 24)& 0xFF),
             DNS_RELAY_PORT);
    return s;
}

/* Forward a single query+response cycle synchronously. The ephemeral
 * outgoing socket is per-query so we never confuse responses from
 * different upstreams across config changes. */
static void forward_one(int listen_sock,
                        uint8_t *qbuf, int qlen,
                        const struct sockaddr_in *client,
                        socklen_t client_len)
{
    uint32_t upstream = pick_upstream();

    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (up < 0) {
        ESP_LOGW(TAG, "upstream socket() failed: errno=%d", errno);
        return;
    }
    struct timeval to = {
        .tv_sec  = DNS_RELAY_UP_TIMEOUT_MS / 1000,
        .tv_usec = (DNS_RELAY_UP_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    struct sockaddr_in up_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_RELAY_PORT),
        .sin_addr.s_addr = upstream,
    };
    int sent = sendto(up, qbuf, qlen, 0,
                      (struct sockaddr *)&up_addr, sizeof(up_addr));
    if (sent < 0) {
        ESP_LOGW(TAG, "upstream sendto() failed: errno=%d", errno);
        close(up);
        return;
    }

    uint8_t rbuf[DNS_RELAY_BUF_SIZE];
    int rlen = recvfrom(up, rbuf, sizeof(rbuf), 0, NULL, NULL);
    close(up);
    if (rlen <= 0) {
        ESP_LOGW(TAG, "upstream recv timeout/error (errno=%d)", errno);
        return;
    }

    int back = sendto(listen_sock, rbuf, rlen, 0,
                      (struct sockaddr *)client, client_len);
    if (back < 0) {
        ESP_LOGW(TAG, "client sendto() failed: errno=%d", errno);
    }
}

static void dns_relay_task(void *arg)
{
    (void)arg;
    int listen_sock = -1;

    /* Settle delay before our first bind: lets WiFi+netif+microlink come
     * up cleanly. The first DNS relay we ever shipped panicked the chip
     * boot-time when we tried to bind 53/udp before WiFi was driving the
     * lwIP stack — this delay sidesteps that race entirely. */
    vTaskDelay(pdMS_TO_TICKS(DNS_RELAY_BOOT_DELAY_MS));
    ESP_LOGI(TAG, "task started (enabled=%d, upstream override=0x%08x)",
             (int)s_enabled, (unsigned)s_upstream_nbo);

    int bind_backoff_ms = 2000;
    while (1) {
        /* (Re-)bind cycle. */
        bool want_listening = s_enabled && s_bind_nbo != 0;
        if (want_listening && (listen_sock < 0 || s_socket_dirty)) {
            if (listen_sock >= 0) { close(listen_sock); listen_sock = -1; }
            listen_sock = open_listen_sock();
            s_socket_dirty = false;
            if (listen_sock < 0) {
                s_healthy = false;
                /* Exponential back-off so a persistently-failing bind
                 * (e.g. AP IP not configured yet) doesn't spam the log. */
                vTaskDelay(pdMS_TO_TICKS(bind_backoff_ms));
                if (bind_backoff_ms < 30000) bind_backoff_ms *= 2;
                continue;
            }
            bind_backoff_ms = 2000;
            bool was_healthy = s_healthy;
            s_healthy = true;
            if (!was_healthy) {
                ESP_LOGI(TAG, "first healthy transition — notifying softap dns reapply");
                dns_relay_on_healthy();
            }
        } else if (!want_listening && listen_sock >= 0) {
            ESP_LOGI(TAG, "disabled — closing listen socket");
            close(listen_sock);
            listen_sock = -1;
            s_healthy = false;
        }

        if (listen_sock < 0) {
            s_healthy = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint8_t qbuf[DNS_RELAY_BUF_SIZE];
        struct sockaddr_in client = {0};
        socklen_t client_len = sizeof(client);
        int qlen = recvfrom(listen_sock, qbuf, sizeof(qbuf), 0,
                            (struct sockaddr *)&client, &client_len);
        if (qlen <= 0) {
            /* timeout or error — fall back to re-check enable state. */
            continue;
        }
        forward_one(listen_sock, qbuf, qlen, &client, client_len);
    }
}

/* ─────────────────── Public API ─────────────────── */

void dns_relay_init(void)
{
    if (s_task) return;

    nvs_handle_t h;
    if (nvs_open(DNS_RELAY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0;
        if (nvs_get_u8(h, KEY_ENABLED, &en) != ESP_OK) en = 0; /* default OFF — operator turns it on from the SPA */
        s_enabled = en != 0;

        char up_str[20] = {0};
        size_t len = sizeof(up_str);
        if (nvs_get_str(h, KEY_UPSTREAM, up_str, &len) == ESP_OK && up_str[0]) {
            ip4_addr_t a;
            if (ip4addr_aton(up_str, &a) && a.addr) {
                s_upstream_nbo = a.addr;
            }
        }
        nvs_close(h);
    } else {
        s_enabled = false;  /* first boot — relay OFF, opt-in only */
    }

    xTaskCreate(dns_relay_task, "dns_relay", DNS_RELAY_TASK_STACK,
                NULL, DNS_RELAY_TASK_PRIO, &s_task);
}

void dns_relay_set_enabled(bool on)
{
    if (s_enabled == on) return;
    s_enabled = on;
    s_socket_dirty = true;

    nvs_handle_t h;
    if (nvs_open(DNS_RELAY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_ENABLED, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "enabled=%d", (int)on);
}

bool dns_relay_is_enabled(void) { return s_enabled; }
bool dns_relay_is_healthy(void) { return s_healthy; }

void dns_relay_set_upstream(uint32_t ip_nbo)
{
    s_upstream_nbo = ip_nbo;

    nvs_handle_t h;
    if (nvs_open(DNS_RELAY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (ip_nbo == 0) {
            nvs_erase_key(h, KEY_UPSTREAM);
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                     (int)( ip_nbo        & 0xFF),
                     (int)((ip_nbo >>  8) & 0xFF),
                     (int)((ip_nbo >> 16) & 0xFF),
                     (int)((ip_nbo >> 24) & 0xFF));
            nvs_set_str(h, KEY_UPSTREAM, buf);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "upstream override=0x%08x", (unsigned)ip_nbo);
}

uint32_t dns_relay_get_upstream(void) { return s_upstream_nbo; }

void dns_relay_set_bind_addr(uint32_t ip_nbo)
{
    if (s_bind_nbo == ip_nbo) return;
    s_bind_nbo = ip_nbo;
    s_socket_dirty = true;
    ESP_LOGI(TAG, "bind addr changed to 0x%08x — rebinding", (unsigned)ip_nbo);
}
