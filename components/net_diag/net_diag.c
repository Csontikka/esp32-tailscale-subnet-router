/* Synchronous ICMP echo / traceroute (see net_diag.h).
 *
 * Built directly on lwIP's BSD-socket layer (SOCK_RAW + IPPROTO_ICMP) so
 * that HTTP handlers can call it synchronously and write the formatted
 * results straight into a response buffer. Matches the source-binding
 * behaviour of the ip4_route_src_hook wrapper in lwip_route_hook.c —
 * locally-originated probes get routed by the same logic as any other
 * ESP-originated socket traffic.
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_diag.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip_addr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "net_diag";

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0
#define ICMP_TIME_EXCEEDED 11
#define ICMP_DEST_UNREACH 3

#define ICMP_HDR_LEN  8
#define ICMP_PAYLOAD_LEN 24
#define ICMP_PROBE_LEN (ICMP_HDR_LEN + ICMP_PAYLOAD_LEN)

#define RECV_BUF_LEN  256

struct icmp_msg {
    uint8_t  type;
    uint8_t  code;
    uint16_t chksum;
    uint16_t id;
    uint16_t seq;
    uint8_t  payload[ICMP_PAYLOAD_LEN];
} __attribute__((packed));

/* Result of one probe. */
typedef struct {
    int     status;        /* 0=reply, 1=ttl-exceeded, -1=timeout, -2=error */
    char    from[INET_ADDRSTRLEN];
    int64_t rtt_us;
} probe_result_t;

static void buf_appendf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void buf_appendf(char *buf, size_t size, const char *fmt, ...)
{
    size_t cur = strlen(buf);
    if (cur >= size) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + cur, size - cur, fmt, ap);
    va_end(ap);
}

static int resolve_target(const char *target, struct sockaddr_in *out_addr,
                          char *out_str, size_t out_str_size)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_RAW,
    };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(target, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        return -1;
    }
    memcpy(out_addr, res->ai_addr, sizeof(*out_addr));
    inet_ntop(AF_INET, &out_addr->sin_addr, out_str, out_str_size);
    freeaddrinfo(res);
    return 0;
}

/* Build an ICMP echo request with identifier=id and sequence=seq.
 * Payload is a recognisable filler so we can match it in TIME_EXCEEDED. */
static void build_echo(struct icmp_msg *msg, uint16_t id, uint16_t seq)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = ICMP_ECHO_REQUEST;
    msg->code = 0;
    msg->id   = htons(id);
    msg->seq  = htons(seq);
    for (int i = 0; i < ICMP_PAYLOAD_LEN; i++) {
        msg->payload[i] = (uint8_t)(i + 'A');
    }
    msg->chksum = 0;
    msg->chksum = inet_chksum(msg, sizeof(*msg));
}

/* Parse a recvfrom buffer (with IPv4 header) and decide whether this is a
 * response to our probe. Returns status as in probe_result_t. */
static int parse_reply(const uint8_t *buf, int len, uint16_t expect_id,
                       uint16_t expect_seq)
{
    if (len < 20 + ICMP_HDR_LEN) return -2;
    int ihl = (buf[0] & 0x0F) * 4;
    if (len < ihl + ICMP_HDR_LEN) return -2;
    const struct icmp_msg *icmp = (const struct icmp_msg *)(buf + ihl);

    if (icmp->type == ICMP_ECHO_REPLY) {
        if (ntohs(icmp->id) == expect_id && ntohs(icmp->seq) == expect_seq) {
            return 0;
        }
        return -2;  /* somebody else's reply */
    }
    if (icmp->type == ICMP_TIME_EXCEEDED || icmp->type == ICMP_DEST_UNREACH) {
        /* Embedded original IP+ICMP starts at icmp->payload. */
        const uint8_t *inner_ip = icmp->payload;
        int inner_avail = len - (ihl + ICMP_HDR_LEN);
        if (inner_avail < 20 + 8) return -2;
        int inner_ihl = (inner_ip[0] & 0x0F) * 4;
        if (inner_avail < inner_ihl + 8) return -2;
        const struct icmp_msg *inner_icmp =
            (const struct icmp_msg *)(inner_ip + inner_ihl);
        if (ntohs(inner_icmp->id) != expect_id ||
            ntohs(inner_icmp->seq) != expect_seq) {
            return -2;
        }
        return (icmp->type == ICMP_TIME_EXCEEDED) ? 1 : -3;
    }
    return -2;
}

/* Send one ICMP probe and wait up to timeout_ms for a matching response.
 * `ttl` of 0 means do not override (kernel default). */
static probe_result_t do_probe(struct sockaddr_in *target, uint16_t id,
                               uint16_t seq, int ttl, int timeout_ms)
{
    probe_result_t r = { .status = -2, .from = "", .rtt_us = 0 };

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: errno=%d", errno);
        return r;
    }

    if (ttl > 0) {
        if (setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            ESP_LOGW(TAG, "setsockopt(IP_TTL=%d) failed: %d", ttl, errno);
        }
    }

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct icmp_msg probe;
    build_echo(&probe, id, seq);

    int64_t t_send = esp_timer_get_time();
    int sent = sendto(sock, &probe, sizeof(probe), 0,
                      (struct sockaddr *)target, sizeof(*target));
    if (sent < 0) {
        ESP_LOGW(TAG, "sendto() failed: errno=%d", errno);
        close(sock);
        return r;
    }

    /* Wait — possibly through unrelated ICMP traffic — for our match. */
    int64_t deadline = t_send + (int64_t)timeout_ms * 1000;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            r.status = -1;
            break;
        }
        int64_t remain_ms = (deadline - now) / 1000;
        if (remain_ms < 1) remain_ms = 1;
        struct timeval tv2 = {
            .tv_sec = remain_ms / 1000,
            .tv_usec = (remain_ms % 1000) * 1000,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));

        uint8_t rxbuf[RECV_BUF_LEN];
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            r.status = -1;  /* timeout */
            break;
        }
        int st = parse_reply(rxbuf, n, id, seq);
        if (st == -2) continue;  /* not for us, keep waiting */
        int64_t t_now = esp_timer_get_time();
        r.status = st;
        r.rtt_us = t_now - t_send;
        inet_ntop(AF_INET, &from.sin_addr, r.from, sizeof(r.from));
        break;
    }

    close(sock);
    return r;
}

int net_diag_ping(const char *target, int count, int timeout_ms,
                  char *out, size_t out_size)
{
    if (!target || !*target || !out || out_size == 0) return -1;
    if (count <= 0) count = 4;
    if (count > 16) count = 16;
    if (timeout_ms <= 0) timeout_ms = 2000;
    if (timeout_ms > 5000) timeout_ms = 5000;
    out[0] = '\0';

    struct sockaddr_in target_addr = {0};
    char target_str[INET_ADDRSTRLEN] = "";
    if (resolve_target(target, &target_addr, target_str, sizeof(target_str)) != 0) {
        buf_appendf(out, out_size, "Cannot resolve %s\n", target);
        return -1;
    }

    buf_appendf(out, out_size, "PING %s (%s) %d data bytes\n",
                target, target_str, ICMP_PAYLOAD_LEN);

    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF);
    int got = 0;
    int64_t rtt_min = -1, rtt_max = 0, rtt_sum = 0;

    for (int i = 0; i < count; i++) {
        probe_result_t r = do_probe(&target_addr, id, (uint16_t)(i + 1), 0,
                                    timeout_ms);
        if (r.status == 0) {
            got++;
            int rtt_ms_int = (int)(r.rtt_us / 1000);
            int rtt_us_rem = (int)((r.rtt_us % 1000) / 100);
            if (rtt_min < 0 || r.rtt_us < rtt_min) rtt_min = r.rtt_us;
            if (r.rtt_us > rtt_max) rtt_max = r.rtt_us;
            rtt_sum += r.rtt_us;
            buf_appendf(out, out_size,
                        "%d bytes from %s: icmp_seq=%d time=%d.%d ms\n",
                        ICMP_PROBE_LEN, r.from, i + 1, rtt_ms_int, rtt_us_rem);
        } else if (r.status == -1) {
            buf_appendf(out, out_size, "icmp_seq=%d timeout\n", i + 1);
        } else if (r.status == -3) {
            buf_appendf(out, out_size,
                        "icmp_seq=%d destination unreachable (from %s)\n",
                        i + 1, r.from);
        } else {
            buf_appendf(out, out_size, "icmp_seq=%d send/recv error\n", i + 1);
        }
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(250));
    }

    int loss_pct = count ? ((count - got) * 100 / count) : 100;
    if (got > 0) {
        int avg_ms_int = (int)((rtt_sum / got) / 1000);
        int avg_us_rem = (int)(((rtt_sum / got) % 1000) / 100);
        buf_appendf(out, out_size,
                    "\n--- %s ping statistics ---\n"
                    "%d packets transmitted, %d received, %d%% packet loss\n"
                    "rtt min/avg/max = %d/%d.%d/%d ms\n",
                    target, count, got, loss_pct,
                    (int)(rtt_min / 1000),
                    avg_ms_int, avg_us_rem,
                    (int)(rtt_max / 1000));
    } else {
        buf_appendf(out, out_size,
                    "\n--- %s ping statistics ---\n"
                    "%d packets transmitted, 0 received, 100%% packet loss\n",
                    target, count);
    }

    return 0;
}

int net_diag_trace(const char *target, int max_hops, int timeout_ms,
                   char *out, size_t out_size)
{
    if (!target || !*target || !out || out_size == 0) return -1;
    if (max_hops <= 0) max_hops = 12;
    if (max_hops > 20) max_hops = 20;
    if (timeout_ms <= 0) timeout_ms = 2000;
    if (timeout_ms > 5000) timeout_ms = 5000;
    out[0] = '\0';

    struct sockaddr_in target_addr = {0};
    char target_str[INET_ADDRSTRLEN] = "";
    if (resolve_target(target, &target_addr, target_str, sizeof(target_str)) != 0) {
        buf_appendf(out, out_size, "Cannot resolve %s\n", target);
        return -1;
    }

    buf_appendf(out, out_size,
                "traceroute to %s (%s), %d hops max\n", target, target_str,
                max_hops);

    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF);

    for (int ttl = 1; ttl <= max_hops; ttl++) {
        probe_result_t r = do_probe(&target_addr, id, (uint16_t)ttl, ttl,
                                    timeout_ms);
        if (r.status == -1) {
            buf_appendf(out, out_size, "%2d  *\n", ttl);
            continue;
        }
        if (r.status < 0) {
            buf_appendf(out, out_size, "%2d  send/recv error\n", ttl);
            continue;
        }
        int rtt_ms_int = (int)(r.rtt_us / 1000);
        int rtt_us_rem = (int)((r.rtt_us % 1000) / 100);
        buf_appendf(out, out_size, "%2d  %-15s  %d.%d ms\n",
                    ttl, r.from, rtt_ms_int, rtt_us_rem);
        if (r.status == 0) {
            /* Echo reply — destination reached. */
            break;
        }
    }

    return 0;
}
