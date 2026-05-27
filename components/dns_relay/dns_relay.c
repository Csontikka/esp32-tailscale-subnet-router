/* DNS forwarder + cache for AP clients.
 *
 * Architecture: one listener task binds AP:53/udp and recvfrom()s client
 * queries. A SPIRAM direct-mapped response cache answers repeats instantly
 * (no upstream round-trip); cache misses are handed over a small work queue
 * to a pool of worker tasks, each owning its own upstream UDP socket. So a
 * single bound listener (one port-53 socket) still serves many concurrent
 * in-flight upstream queries, and a slow/stuck upstream query ties up only
 * one worker — not the whole relay.
 *
 * The cache lives in SPIRAM (heap_caps), and per-miss work items are SPIRAM-
 * allocated, so the relay's internal-DRAM footprint is just the three small
 * task stacks. Lookup is O(1): FNV-1a hash of the question -> one slot, with
 * a full-question memcmp so a hash collision is a miss, never a wrong answer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dns_relay.h"

#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DNS_RELAY_PORT          53
#define DNS_RELAY_BUF_SIZE      1024     /* RFC 1035 §2.3.4: 512 udp, EDNS0 lifts it; 1024 ceiling. */
#define DNS_RELAY_UP_TIMEOUT_MS 3500
#define DNS_RELAY_BOOT_DELAY_MS 3000     /* Settle window before the first bind attempt. */

#define DNS_LISTENER_STACK      4096
#define DNS_WORKER_STACK        4608
#define DNS_RELAY_TASK_PRIO     4
#define DNS_WORKER_COUNT        2
#define DNS_WORK_QUEUE_DEPTH    8

/* SPIRAM direct-mapped response cache. 256 entries * ~1.3 KB ~= 330 KB SPIRAM. */
#define DNS_CACHE_ENTRIES       256
#define DNS_CACHE_KEY_MAX       256      /* DNS question section ceiling (qname<=255 + qtype/qclass) */
#define DNS_CACHE_TTL_MIN_S     10
#define DNS_CACHE_TTL_MAX_S     3600
#define DNS_CACHE_TTL_NEG_S     15       /* empty / NXDOMAIN brief negative cache */

#define DNS_RELAY_NVS_NS        "tsr"
#define KEY_ENABLED             "dns_relay_en"
#define KEY_UPSTREAM            "dns_relay_up"

#define DEFAULT_FALLBACK_UPSTREAM 0x01010101UL  /* 1.1.1.1, network order written below. */

static const char *TAG = "dns_relay";

static volatile bool     s_enabled       = false;
static volatile uint32_t s_upstream_nbo  = 0;      /* 0 = use STA-learned */
static volatile uint32_t s_bind_nbo      = 0;      /* AP IP; 0 = wait, don't bind yet */
static volatile bool     s_socket_dirty  = false;  /* tell the listener to rebind */
static volatile bool     s_healthy       = false;  /* bound and serving — softap_set_dns_addr gates on this */
static volatile int      s_listen_sock   = -1;     /* listener-owned; workers reply through it */

static TaskHandle_t s_listener = NULL;

/* Explicit registration-based health callback (see git history: weak-symbol
 * overrides didn't resolve reliably across the component link boundary). */
static dns_relay_state_cb_t s_state_cb = NULL;
void dns_relay_set_state_cb(dns_relay_state_cb_t cb) { s_state_cb = cb; }
static void notify_state(bool healthy)
{
    dns_relay_state_cb_t cb = s_state_cb;
    if (cb) cb(healthy);
}

/* ---- listener -> worker hand-off: a pointer queue; items live in SPIRAM ---- */
typedef struct {
    struct sockaddr_in client;
    socklen_t          client_len;
    int                qlen;
    uint8_t            q[DNS_RELAY_BUF_SIZE];
} dns_work_t;
static QueueHandle_t s_workq = NULL;

/* ---- SPIRAM direct-mapped response cache ---- */
typedef struct {
    int64_t  expiry_us;                 /* esp_timer_get_time() deadline; 0 = empty slot */
    uint16_t key_len;
    uint16_t resp_len;
    uint8_t  key[DNS_CACHE_KEY_MAX];    /* question section (from offset 12): qname+qtype+qclass */
    uint8_t  resp[DNS_RELAY_BUF_SIZE];  /* full upstream response */
} dns_cache_entry_t;
static dns_cache_entry_t *s_cache = NULL;       /* SPIRAM */
static SemaphoreHandle_t  s_cache_mtx = NULL;

/* Diagnostics counters. Unlocked: a torn increment only skews a stat by one,
 * which is irrelevant — the cache *data* is what the mutex protects. */
static volatile uint32_t st_queries, st_hits, st_misses, st_inserts, st_evictions;

/* Resolve the live upstream IP every forward. Priority: explicit override,
 * then STA-learned DNS, then 1.1.1.1 (so we never serve garbage). */
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

/* ───────────────────── DNS packet helpers ───────────────────── */

/* Byte length of the question section starting at offset 12 (qname labels +
 * qtype + qclass), or 0 if malformed / oversized / not a single clean
 * question. Compression pointers are illegal in a question -> reject. */
static int dns_question_len(const uint8_t *p, int len)
{
    if (len < 12 + 5) return 0;          /* header + at least root(1) + qtype/qclass(4) */
    int i = 12;
    while (i < len) {
        uint8_t c = p[i];
        if (c == 0) { i += 1 + 4; break; }   /* root label terminator + qtype + qclass */
        if (c & 0xC0) return 0;              /* compression / reserved in a question */
        i += 1 + c;
    }
    if (i > len) return 0;
    int qlen = i - 12;
    return (qlen >= 5 && qlen <= DNS_CACHE_KEY_MAX) ? qlen : 0;
}

/* Cacheable standard query? QR=0, opcode=0, QDCOUNT==1. */
static bool dns_is_std_query(const uint8_t *p, int len)
{
    if (len < 12) return false;
    if (p[2] & 0x80) return false;             /* QR=1 => a response, not a query */
    if ((p[2] >> 3) & 0x0F) return false;      /* opcode != 0 (QUERY) */
    return ((p[4] << 8) | p[5]) == 1;          /* QDCOUNT == 1 */
}

/* Cache TTL for a response: min TTL across the answer RRs, clamped. Returns 0
 * if the response must not be cached (RCODE other than NOERROR/NXDOMAIN). An
 * empty answer section (incl. NXDOMAIN) gets a brief negative-cache TTL.
 * qlen is the question length from dns_question_len(). */
static uint32_t dns_resp_ttl(const uint8_t *p, int len, int qlen)
{
    uint8_t rcode = p[3] & 0x0F;
    if (rcode != 0 && rcode != 3) return 0;    /* only NOERROR / NXDOMAIN */
    uint16_t an = (p[6] << 8) | p[7];
    if (an == 0) return DNS_CACHE_TTL_NEG_S;
    int i = 12 + qlen;                          /* start of answer section */
    uint32_t minttl = 0xFFFFFFFFu;
    for (int a = 0; a < an; a++) {
        if (i + 1 > len) return DNS_CACHE_TTL_NEG_S;
        if ((p[i] & 0xC0) == 0xC0) {            /* compressed name -> 2 bytes */
            i += 2;
        } else {                                /* uncompressed labels */
            while (i < len && p[i]) i += 1 + p[i];
            i += 1;
        }
        if (i + 10 > len) return DNS_CACHE_TTL_NEG_S;
        uint32_t ttl = ((uint32_t)p[i+4] << 24) | ((uint32_t)p[i+5] << 16) |
                       ((uint32_t)p[i+6] << 8)  |  (uint32_t)p[i+7];
        uint16_t rdlen = (p[i+8] << 8) | p[i+9];
        if (ttl < minttl) minttl = ttl;
        i += 10 + rdlen;
    }
    if (minttl == 0xFFFFFFFFu) return DNS_CACHE_TTL_NEG_S;
    if (minttl < DNS_CACHE_TTL_MIN_S) minttl = DNS_CACHE_TTL_MIN_S;
    if (minttl > DNS_CACHE_TTL_MAX_S) minttl = DNS_CACHE_TTL_MAX_S;
    return minttl;
}

static uint32_t fnv1a(const uint8_t *d, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* Look the question up. On hit, copies the response to out (*out_len set) and
 * rewrites the txid to the querying client's. Returns true on hit. */
static bool cache_get(const uint8_t *q, int key_len, uint8_t *out, int *out_len)
{
    if (!s_cache || !s_cache_mtx) return false;
    uint32_t slot = fnv1a(q + 12, key_len) % DNS_CACHE_ENTRIES;
    bool hit = false;
    xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
    dns_cache_entry_t *e = &s_cache[slot];
    if (e->expiry_us != 0 && e->key_len == key_len &&
        esp_timer_get_time() < e->expiry_us &&
        memcmp(e->key, q + 12, key_len) == 0) {
        memcpy(out, e->resp, e->resp_len);
        *out_len = e->resp_len;
        hit = true;
    }
    xSemaphoreGive(s_cache_mtx);
    if (hit) { out[0] = q[0]; out[1] = q[1]; }   /* match the client's transaction id */
    return hit;
}

static void cache_put(const uint8_t *q, int key_len,
                      const uint8_t *resp, int resp_len, uint32_t ttl_s)
{
    if (!s_cache || !s_cache_mtx) return;
    if (key_len <= 0 || key_len > DNS_CACHE_KEY_MAX) return;
    if (resp_len <= 0 || resp_len > DNS_RELAY_BUF_SIZE) return;
    uint32_t slot = fnv1a(q + 12, key_len) % DNS_CACHE_ENTRIES;
    xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
    dns_cache_entry_t *e = &s_cache[slot];
    bool was_other = (e->expiry_us != 0 &&
                      !(e->key_len == key_len && memcmp(e->key, q + 12, key_len) == 0));
    e->expiry_us = esp_timer_get_time() + (int64_t)ttl_s * 1000000;
    e->key_len   = (uint16_t)key_len;
    memcpy(e->key, q + 12, key_len);
    e->resp_len  = (uint16_t)resp_len;
    memcpy(e->resp, resp, resp_len);
    xSemaphoreGive(s_cache_mtx);
    st_inserts++;
    if (was_other) st_evictions++;
}

/* ───────────────────── Worker pool ───────────────────── */

/* Each worker owns one upstream UDP socket so concurrent in-flight queries
 * never cross responses. A send/recv hard error rebuilds just this socket. */
static void worker_task(void *arg)
{
    (void)arg;
    int up = -1;
    uint8_t *rbuf = heap_caps_malloc(DNS_RELAY_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rbuf) rbuf = malloc(DNS_RELAY_BUF_SIZE);   /* SPIRAM preferred; fall back to internal */
    if (!rbuf) { ESP_LOGE(TAG, "worker rbuf alloc failed"); vTaskDelete(NULL); return; }

    for (;;) {
        dns_work_t *w = NULL;
        if (xQueueReceive(s_workq, &w, portMAX_DELAY) != pdTRUE || !w) continue;

        if (up < 0) {
            up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (up >= 0) {
                struct timeval to = {
                    .tv_sec  = DNS_RELAY_UP_TIMEOUT_MS / 1000,
                    .tv_usec = (DNS_RELAY_UP_TIMEOUT_MS % 1000) * 1000,
                };
                setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
            }
        }
        if (up < 0) { heap_caps_free(w); continue; }

        struct sockaddr_in up_addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(DNS_RELAY_PORT),
            .sin_addr.s_addr = pick_upstream(),
        };
        if (sendto(up, w->q, w->qlen, 0, (struct sockaddr *)&up_addr, sizeof(up_addr)) < 0) {
            ESP_LOGW(TAG, "upstream sendto() failed: errno=%d", errno);
            close(up); up = -1;
            heap_caps_free(w);
            continue;
        }
        int rlen = recvfrom(up, rbuf, DNS_RELAY_BUF_SIZE, 0, NULL, NULL);
        if (rlen <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) { close(up); up = -1; }
            heap_caps_free(w);
            continue;
        }

        int ls = s_listen_sock;            /* listener-owned; bound to AP:53 */
        if (ls >= 0) {
            sendto(ls, rbuf, rlen, 0, (struct sockaddr *)&w->client, w->client_len);
        }

        /* Cache the answer if it's a cacheable standard query/response. */
        int kl = dns_is_std_query(w->q, w->qlen) ? dns_question_len(w->q, w->qlen) : 0;
        if (kl > 0) {
            uint32_t ttl = dns_resp_ttl(rbuf, rlen, kl);
            if (ttl) cache_put(w->q, kl, rbuf, rlen, ttl);
        }
        heap_caps_free(w);
    }
}

/* ───────────────────── Listener ───────────────────── */

static void listener_task(void *arg)
{
    (void)arg;
    int listen_sock = -1;
    uint8_t qbuf[DNS_RELAY_BUF_SIZE];
    uint8_t cbuf[DNS_RELAY_BUF_SIZE];      /* cache-hit response scratch */

    /* Settle delay before the first bind: the very first DNS relay we shipped
     * panicked at boot trying to bind 53/udp before WiFi drove lwIP. */
    vTaskDelay(pdMS_TO_TICKS(DNS_RELAY_BOOT_DELAY_MS));
    ESP_LOGI(TAG, "task started (enabled=%d, upstream override=0x%08x, cache=%s)",
             (int)s_enabled, (unsigned)s_upstream_nbo, s_cache ? "on" : "off");

    int bind_backoff_ms = 2000;
    for (;;) {
        bool want_listening = s_enabled && s_bind_nbo != 0;
        if (want_listening && (listen_sock < 0 || s_socket_dirty)) {
            if (listen_sock >= 0) { s_listen_sock = -1; close(listen_sock); listen_sock = -1; }
            listen_sock = open_listen_sock();
            s_socket_dirty = false;
            if (listen_sock < 0) {
                s_healthy = false;
                vTaskDelay(pdMS_TO_TICKS(bind_backoff_ms));
                if (bind_backoff_ms < 30000) bind_backoff_ms *= 2;
                continue;
            }
            bind_backoff_ms = 2000;
            s_listen_sock = listen_sock;
            bool was_healthy = s_healthy;
            s_healthy = true;
            if (!was_healthy) {
                ESP_LOGI(TAG, "first healthy transition — notifying softap dns reapply");
                notify_state(true);
            }
        } else if (!want_listening && listen_sock >= 0) {
            ESP_LOGI(TAG, "disabled — closing listen socket");
            s_listen_sock = -1;
            close(listen_sock);
            listen_sock = -1;
            bool was_healthy = s_healthy;
            s_healthy = false;
            if (was_healthy) notify_state(false);
        }

        if (listen_sock < 0) {
            s_healthy = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        struct sockaddr_in client = {0};
        socklen_t client_len = sizeof(client);
        int qlen = recvfrom(listen_sock, qbuf, sizeof(qbuf), 0,
                            (struct sockaddr *)&client, &client_len);
        if (qlen <= 0) continue;            /* timeout -> re-check enable state */

        st_queries++;

        /* Cache fast-path: serve repeats straight from SPIRAM, no worker. */
        int kl = dns_is_std_query(qbuf, qlen) ? dns_question_len(qbuf, qlen) : 0;
        if (kl > 0) {
            int rl = 0;
            if (cache_get(qbuf, kl, cbuf, &rl)) {
                st_hits++;
                sendto(listen_sock, cbuf, rl, 0, (struct sockaddr *)&client, client_len);
                continue;
            }
        }

        /* Miss (or uncacheable): dispatch to a worker. */
        st_misses++;
        dns_work_t *w = heap_caps_malloc(sizeof(dns_work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!w) continue;                   /* out of SPIRAM -> drop; client retries */
        w->client     = client;
        w->client_len = client_len;
        w->qlen       = qlen;
        memcpy(w->q, qbuf, qlen);
        if (xQueueSend(s_workq, &w, 0) != pdTRUE) {
            heap_caps_free(w);              /* both workers busy + queue full -> drop */
        }
    }
}

/* ───────────────────── Public API ───────────────────── */

void dns_relay_init(void)
{
    if (s_listener) return;

    nvs_handle_t h;
    if (nvs_open(DNS_RELAY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 1;
        if (nvs_get_u8(h, KEY_ENABLED, &en) != ESP_OK) en = 1;
        s_enabled = en != 0;

        char up_str[20] = {0};
        size_t len = sizeof(up_str);
        if (nvs_get_str(h, KEY_UPSTREAM, up_str, &len) == ESP_OK && up_str[0]) {
            ip4_addr_t a;
            if (ip4addr_aton(up_str, &a) && a.addr) s_upstream_nbo = a.addr;
        }
        nvs_close(h);
    } else {
        s_enabled = true;
    }

    /* SPIRAM response cache + hand-off queue. If SPIRAM is exhausted the relay
     * still works, just cache-less (cache_get/put degrade to no-ops). */
    s_cache = heap_caps_calloc(DNS_CACHE_ENTRIES, sizeof(dns_cache_entry_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cache) ESP_LOGW(TAG, "SPIRAM cache alloc failed — running cache-less");
    s_cache_mtx = xSemaphoreCreateMutex();
    s_workq     = xQueueCreate(DNS_WORK_QUEUE_DEPTH, sizeof(dns_work_t *));
    if (!s_workq) { ESP_LOGE(TAG, "work queue alloc failed"); return; }

    xTaskCreate(listener_task, "dns_relay", DNS_LISTENER_STACK, NULL,
                DNS_RELAY_TASK_PRIO, &s_listener);
    for (int i = 0; i < DNS_WORKER_COUNT; i++) {
        xTaskCreate(worker_task, "dns_worker", DNS_WORKER_STACK, NULL,
                    DNS_RELAY_TASK_PRIO, NULL);
    }
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

void dns_relay_get_stats(dns_relay_stats_t *out)
{
    if (!out) return;
    out->queries     = st_queries;
    out->hits        = st_hits;
    out->misses      = st_misses;
    out->inserts     = st_inserts;
    out->evictions   = st_evictions;
    out->capacity    = DNS_CACHE_ENTRIES;
    out->cache_bytes = s_cache ? (uint32_t)(DNS_CACHE_ENTRIES * sizeof(dns_cache_entry_t)) : 0;
    out->enabled     = s_enabled ? 1 : 0;
    out->healthy     = s_healthy ? 1 : 0;

    uint32_t denom = st_hits + st_misses;
    out->hit_pct = denom ? (uint32_t)(((uint64_t)st_hits * 100) / denom) : 0;

    uint32_t used = 0;
    if (s_cache && s_cache_mtx) {
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
        for (int i = 0; i < DNS_CACHE_ENTRIES; i++)
            if (s_cache[i].expiry_us > now) used++;
        xSemaphoreGive(s_cache_mtx);
    }
    out->entries = used;
}
