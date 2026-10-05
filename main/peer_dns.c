/* DNS-over-HTTP service for Tailscale peers using this node as an exit node.
 *
 * Native Tailscale clients send DNS wire messages to the PeerAPI address the
 * node advertises: http://<tailnet-ip>:<TAILSCALE_PEERAPI_PORT>/dns-query.
 * The service has its own small HTTP server and runs only while the router
 * offers itself as an exit node: the admin web server handles one request at
 * a time, and a page load on a client is dozens of lookups, so sharing it
 * would stall the UI. Requests are authorised from the accepted socket's
 * addresses before any header, query parameter or body is read.
 *
 * Lookups go to the uplink resolver (or the DNS forwarder's configured
 * upstream): UDP first, TCP when the answer is truncated.
 *
 * SPDX-License-Identifier: MIT
 */

#include "peer_dns.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dns_relay.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "microlink.h"
#include "tailscale_config.h"

#define PEER_DNS_MAX_MESSAGE       4096U
#define PEER_DNS_MAX_B64           (((PEER_DNS_MAX_MESSAGE + 2U) / 3U) * 4U)
#define PEER_DNS_MAX_QUERY_STRING  (PEER_DNS_MAX_B64 + 32U)
#define PEER_DNS_TIMEOUT_US        (3000LL * 1000LL)
#define PEER_DNS_UDP_TIMEOUT_US    (1500LL * 1000LL)

static const char *TAG = "peer_dns";

typedef enum {
    UPSTREAM_OK = 0,
    UPSTREAM_TIMEOUT,
    UPSTREAM_ERROR,
    UPSTREAM_NO_MEMORY,
} upstream_result_t;

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    (void)httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
    /* Closing after an error also discards an unread POST body safely. */
    return ESP_FAIL;
}

static bool advertises_exit_node(void)
{
    return tailscale_exit_server_active();
}

/* ESP-IDF's HTTP server listens with PF_INET6 when lwIP IPv6 support is
 * enabled, so an IPv4 connection is exposed by getsockname/getpeername as an
 * IPv4-mapped IPv6 sockaddr.  Normalize only the two IPv4 representations;
 * native IPv6 remains outside this IPv4 PeerAPI contract. */
static bool sockaddr_ipv4_host(const struct sockaddr_storage *address,
                               uint32_t *ipv4_host)
{
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        *ipv4_host = ntohl(v4->sin_addr.s_addr);
        return true;
    }
    if (address->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        if (!IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) return false;
        uint32_t ipv4_network;
        memcpy(&ipv4_network, &v6->sin6_addr.s6_addr[12], sizeof(ipv4_network));
        *ipv4_host = ntohl(ipv4_network);
        return true;
    }
    return false;
}

/* Requests are accepted only on the tunnel address and only when the remote
 * address exactly matches a peer currently known to microlink.  Merely being
 * in 100.64.0.0/10 is insufficient, so AP/LAN requests to port 80 cannot use
 * this unauthenticated endpoint. */
static bool peer_request_allowed(httpd_req_t *req)
{
    if (!advertises_exit_node()) return false;

    microlink_t *ml = tailscale_get_microlink();
    if (!ml || !microlink_is_connected(ml)) return false;

    uint32_t own_ip = microlink_get_vpn_ip(ml); /* host byte order */
    if (own_ip == 0) return false;

    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;

    struct sockaddr_storage local = {0};
    struct sockaddr_storage remote = {0};
    socklen_t local_len = sizeof(local);
    socklen_t remote_len = sizeof(remote);
    uint32_t local_ip;
    uint32_t remote_ip;
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
        getpeername(fd, (struct sockaddr *)&remote, &remote_len) != 0 ||
        !sockaddr_ipv4_host(&local, &local_ip) ||
        !sockaddr_ipv4_host(&remote, &remote_ip) ||
        local_ip != own_ip) {
        return false;
    }

    int count = microlink_get_peer_count(ml);
    for (int i = 0; i < count; i++) {
        microlink_peer_info_t peer;
        if (microlink_get_peer_info(ml, i, &peer) == ESP_OK &&
            peer.vpn_ip != 0 && peer.vpn_ip == remote_ip) {
            return true;
        }
    }
    return false;
}

static bool dns_query_valid(const uint8_t *message, size_t length)
{
    /* A DNS header is 12 bytes and a query must have QR clear. */
    return message && length >= 12 && length <= PEER_DNS_MAX_MESSAGE &&
           (message[2] & 0x80U) == 0;
}

static int base64url_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* RFC 8484 GET uses unpadded base64url.  Reject non-canonical trailing bits
 * rather than silently accepting a second spelling of the same DNS packet. */
static bool decode_base64url(const char *encoded, size_t encoded_len,
                             uint8_t *decoded, size_t decoded_cap,
                             size_t *decoded_len)
{
    if (!encoded || encoded_len == 0 || encoded_len > PEER_DNS_MAX_B64 ||
        (encoded_len & 3U) == 1U) {
        return false;
    }

    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t out = 0;
    for (size_t i = 0; i < encoded_len; i++) {
        int value = base64url_value((unsigned char)encoded[i]);
        if (value < 0) return false; /* Padding is forbidden too. */
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= decoded_cap) return false;
            decoded[out++] = (uint8_t)(accumulator >> bits);
        }
    }
    if (bits != 0 && (accumulator & ((1U << bits) - 1U)) != 0) return false;
    *decoded_len = out;
    return true;
}

static bool resolver_is_local(uint32_t resolver_nbo)
{
    esp_netif_t *interfaces[] = {
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
        esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
    };
    for (size_t i = 0; i < sizeof(interfaces) / sizeof(interfaces[0]); i++) {
        if (!interfaces[i]) continue;
        esp_netif_ip_info_t info = {0};
        if (esp_netif_get_ip_info(interfaces[i], &info) == ESP_OK &&
            info.ip.addr != 0 && info.ip.addr == resolver_nbo) {
            return true;
        }
    }

    microlink_t *ml = tailscale_get_microlink();
    return ml && microlink_get_vpn_ip(ml) == ntohl(resolver_nbo);
}

static bool resolver_is_usable(uint32_t resolver_nbo)
{
    uint32_t host = ntohl(resolver_nbo);
    if (host == 0 || host == UINT32_MAX) return false;
    if ((host & 0xFF000000U) == 0x00000000U || /* this network */
        (host & 0xFF000000U) == 0x7F000000U || /* loopback */
        (host & 0xF0000000U) == 0xE0000000U) { /* multicast/reserved */
        return false;
    }
    return !resolver_is_local(resolver_nbo);
}

/* Use an explicit DNS-relay upstream first, otherwise the resolver currently
 * installed on the STA netif.  There is intentionally no public fallback:
 * an absent or unsafe resolver is a configuration error, not permission to
 * send DNS traffic somewhere the operator did not choose. */
static uint32_t select_resolver(void)
{
    uint32_t resolver = dns_relay_get_upstream();
    if (resolver == 0) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dns_info_t info = {0};
        if (!sta || esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &info) != ESP_OK ||
            info.ip.type != ESP_IPADDR_TYPE_V4) {
            return 0;
        }
        resolver = info.ip.u_addr.ip4.addr;
    }
    return resolver_is_usable(resolver) ? resolver : 0;
}

static int wait_for_socket(int fd, bool write_ready, int64_t deadline_us)
{
    for (;;) {
        int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) return 0;

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if (write_ready) FD_SET(fd, &write_set);
        else FD_SET(fd, &read_set);
        struct timeval timeout = {
            .tv_sec = (long)(remaining / 1000000LL),
            .tv_usec = (long)(remaining % 1000000LL),
        };
        int rc = select(fd + 1, write_ready ? NULL : &read_set,
                        write_ready ? &write_set : NULL, NULL, &timeout);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

static upstream_result_t socket_write_all(int fd, const uint8_t *data,
                                          size_t length, int64_t deadline_us)
{
    size_t sent = 0;
    while (sent < length) {
        int ready = wait_for_socket(fd, true, deadline_us);
        if (ready == 0) return UPSTREAM_TIMEOUT;
        if (ready < 0) return UPSTREAM_ERROR;
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
            continue;
        } else {
            return UPSTREAM_ERROR;
        }
    }
    return UPSTREAM_OK;
}

static upstream_result_t socket_read_all(int fd, uint8_t *data,
                                         size_t length, int64_t deadline_us)
{
    size_t received = 0;
    while (received < length) {
        int ready = wait_for_socket(fd, false, deadline_us);
        if (ready == 0) return UPSTREAM_TIMEOUT;
        if (ready < 0) return UPSTREAM_ERROR;
        ssize_t n = recv(fd, data + received, length - received, 0);
        if (n > 0) {
            received += (size_t)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
            continue;
        } else {
            return UPSTREAM_ERROR;
        }
    }
    return UPSTREAM_OK;
}

/* One UDP exchange with `server` (network byte order). A truncated answer is
 * reported as such so the caller can repeat the question over TCP. */
static upstream_result_t forward_dns_udp(uint32_t server, const uint8_t *query,
                                         size_t query_len, uint8_t **response,
                                         size_t *response_len, bool *truncated)
{
    *truncated = false;
    uint8_t *answer = malloc(PEER_DNS_MAX_MESSAGE);
    if (!answer) return UPSTREAM_NO_MEMORY;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        free(answer);
        return UPSTREAM_ERROR;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = server,
    };
    upstream_result_t result = UPSTREAM_OK;
    if (sendto(fd, query, query_len, 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)query_len) {
        result = UPSTREAM_ERROR;
    }
    int64_t deadline = esp_timer_get_time() + PEER_DNS_UDP_TIMEOUT_US;
    ssize_t n = 0;
    while (result == UPSTREAM_OK) {
        int ready = wait_for_socket(fd, false, deadline);
        if (ready <= 0) {
            result = ready == 0 ? UPSTREAM_TIMEOUT : UPSTREAM_ERROR;
            break;
        }
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        n = recvfrom(fd, answer, PEER_DNS_MAX_MESSAGE, 0,
                     (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            result = UPSTREAM_ERROR;
            break;
        }
        /* Only the server we asked, answering this question. */
        if (from.sin_addr.s_addr == server && n >= 12 &&
            answer[0] == query[0] && answer[1] == query[1] &&
            (answer[2] & 0x80U) != 0) {
            break;
        }
    }
    close(fd);
    if (result != UPSTREAM_OK) {
        free(answer);
        return result;
    }
    if (answer[2] & 0x02U) {   /* TC */
        *truncated = true;
        free(answer);
        return UPSTREAM_OK;
    }
    *response = answer;
    *response_len = (size_t)n;
    return UPSTREAM_OK;
}

static upstream_result_t forward_dns_tcp(const uint8_t *query, size_t query_len,
                                         uint8_t **response, size_t *response_len);

/* UDP to the resolver, TCP only for a truncated answer or when UDP got
 * nothing: one datagram each way is what almost every lookup needs, and a TCP
 * connection per question tripled the time a client waits. */
static upstream_result_t forward_dns(const uint8_t *query, size_t query_len,
                                     uint8_t **response, size_t *response_len)
{
    uint32_t resolver = select_resolver();
    if (resolver == 0) return UPSTREAM_ERROR;
    bool truncated = false;
    upstream_result_t result = forward_dns_udp(resolver, query, query_len,
                                               response, response_len, &truncated);
    if (result == UPSTREAM_OK && !truncated) return UPSTREAM_OK;
    if (result == UPSTREAM_NO_MEMORY) return result;
    return forward_dns_tcp(query, query_len, response, response_len);
}

static upstream_result_t forward_dns_tcp(const uint8_t *query, size_t query_len,
                                         uint8_t **response, size_t *response_len)
{
    uint32_t resolver = select_resolver();
    if (resolver == 0) return UPSTREAM_ERROR;

    uint8_t *answer = malloc(PEER_DNS_MAX_MESSAGE);
    if (!answer) return UPSTREAM_NO_MEMORY;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        free(answer);
        return UPSTREAM_ERROR;
    }

    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) != 0) {
        close(fd);
        free(answer);
        return UPSTREAM_ERROR;
    }

    struct sockaddr_in upstream = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = resolver,
    };
    int64_t deadline = esp_timer_get_time() + PEER_DNS_TIMEOUT_US;
    if (connect(fd, (struct sockaddr *)&upstream, sizeof(upstream)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            free(answer);
            return UPSTREAM_ERROR;
        }
        int ready = wait_for_socket(fd, true, deadline);
        if (ready <= 0) {
            close(fd);
            free(answer);
            return ready == 0 ? UPSTREAM_TIMEOUT : UPSTREAM_ERROR;
        }
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
            socket_error != 0) {
            close(fd);
            free(answer);
            return socket_error == ETIMEDOUT ? UPSTREAM_TIMEOUT : UPSTREAM_ERROR;
        }
    }

    uint8_t length_prefix[2] = {
        (uint8_t)(query_len >> 8),
        (uint8_t)(query_len & 0xFFU),
    };
    upstream_result_t result = socket_write_all(fd, length_prefix,
                                                sizeof(length_prefix), deadline);
    if (result == UPSTREAM_OK) {
        result = socket_write_all(fd, query, query_len, deadline);
    }

    uint8_t response_prefix[2] = {0};
    if (result == UPSTREAM_OK) {
        result = socket_read_all(fd, response_prefix, sizeof(response_prefix), deadline);
    }
    size_t wire_len = ((size_t)response_prefix[0] << 8) | response_prefix[1];
    if (result == UPSTREAM_OK &&
        (wire_len < 12 || wire_len > PEER_DNS_MAX_MESSAGE)) {
        result = UPSTREAM_ERROR;
    }
    if (result == UPSTREAM_OK) {
        result = socket_read_all(fd, answer, wire_len, deadline);
    }
    close(fd);

    if (result == UPSTREAM_OK &&
        (answer[0] != query[0] || answer[1] != query[1] ||
         (answer[2] & 0x80U) == 0)) {
        result = UPSTREAM_ERROR;
    }
    if (result != UPSTREAM_OK) {
        free(answer);
        return result;
    }

    *response = answer;
    *response_len = wire_len;
    return UPSTREAM_OK;
}

static esp_err_t send_dns_response(httpd_req_t *req, const uint8_t *query,
                                   size_t query_len)
{
    uint8_t *response = NULL;
    size_t response_len = 0;
    upstream_result_t result = forward_dns(query, query_len,
                                           &response, &response_len);
    if (result == UPSTREAM_TIMEOUT) {
        return send_error(req, "504 Gateway Timeout", "DNS upstream timed out");
    }
    if (result == UPSTREAM_NO_MEMORY) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    if (result != UPSTREAM_OK) {
        return send_error(req, "502 Bad Gateway", "DNS upstream failed");
    }

    httpd_resp_set_type(req, "application/dns-message");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, (const char *)response, response_len);
    free(response);
    return err;
}

static esp_err_t peer_dns_get_handler(httpd_req_t *req)
{
    if (!peer_request_allowed(req)) {
        return send_error(req, "403 Forbidden", "forbidden");
    }

    size_t query_string_len = httpd_req_get_url_query_len(req);
    if (query_string_len == 0 || query_string_len > PEER_DNS_MAX_QUERY_STRING) {
        return send_error(req, "400 Bad Request", "invalid dns query");
    }
    char *query_string = malloc(query_string_len + 1);
    uint8_t *query = malloc(PEER_DNS_MAX_MESSAGE);
    if (!query_string || !query) {
        free(query_string);
        free(query);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    if (httpd_req_get_url_query_str(req, query_string,
                                    query_string_len + 1) != ESP_OK) {
        free(query_string);
        free(query);
        return send_error(req, "400 Bad Request", "invalid dns query");
    }

    const char *encoded = NULL;
    size_t encoded_len = 0;
    for (char *part = query_string; part && *part;) {
        char *next = strchr(part, '&');
        if (next) *next = '\0';
        if (strncmp(part, "dns=", 4) == 0) {
            if (encoded) {
                free(query_string);
                free(query);
                return send_error(req, "400 Bad Request", "duplicate dns parameter");
            }
            encoded = part + 4;
            encoded_len = strlen(encoded);
        }
        part = next ? next + 1 : NULL;
    }

    size_t query_len = 0;
    bool valid = encoded && decode_base64url(encoded, encoded_len, query,
                                             PEER_DNS_MAX_MESSAGE, &query_len) &&
                 dns_query_valid(query, query_len);
    free(query_string);
    if (!valid) {
        free(query);
        return send_error(req, "400 Bad Request", "invalid dns query");
    }
    esp_err_t err = send_dns_response(req, query, query_len);
    free(query);
    return err;
}

static bool content_type_is_dns_message(httpd_req_t *req)
{
    size_t length = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (length == 0 || length > 96) return false;
    char *value = malloc(length + 1);
    if (!value) return false;
    bool matches = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", value, length + 1) == ESP_OK) {
        char *start = value;
        while (*start == ' ' || *start == '\t') start++;
        char *end = start;
        while (*end && *end != ';' && *end != ' ' && *end != '\t') end++;
        matches = (size_t)(end - start) == sizeof("application/dns-message") - 1 &&
                  strncasecmp(start, "application/dns-message",
                              sizeof("application/dns-message") - 1) == 0;
    }
    free(value);
    return matches;
}

static esp_err_t peer_dns_post_handler(httpd_req_t *req)
{
    if (!peer_request_allowed(req)) {
        return send_error(req, "403 Forbidden", "forbidden");
    }
    if (!content_type_is_dns_message(req)) {
        return send_error(req, "415 Unsupported Media Type",
                          "Content-Type must be application/dns-message");
    }
    if (req->content_len < 12 || req->content_len > PEER_DNS_MAX_MESSAGE) {
        return send_error(req, "400 Bad Request", "invalid dns query");
    }

    uint8_t *query = malloc(req->content_len);
    if (!query) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, (char *)query + received,
                               req->content_len - received);
        if (n <= 0) {
            free(query);
            return send_error(req, "400 Bad Request", "incomplete dns query");
        }
        received += (size_t)n;
    }
    if (!dns_query_valid(query, received)) {
        free(query);
        return send_error(req, "400 Bad Request", "invalid dns query");
    }

    esp_err_t err = send_dns_response(req, query, received);
    free(query);
    return err;
}

void peer_dns_start(void)
{
    static httpd_handle_t server = NULL;
    if (server || !tailscale_exit_server_active()) return;

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port      = TAILSCALE_PEERAPI_PORT;
    conf.ctrl_port        = (uint16_t)(conf.ctrl_port + 1);   /* the admin server owns the default */
    conf.max_uri_handlers = 2;
    conf.max_open_sockets = 3;
    conf.lru_purge_enable = true;
    conf.stack_size       = 6144;
    /* PSRAM stack: internal RAM is the scarce pool on this board. */
    conf.task_caps        = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (httpd_start(&server, &conf) != ESP_OK) {
        ESP_LOGE(TAG, "exit-node DNS service failed to start on port %d",
                 TAILSCALE_PEERAPI_PORT);
        server = NULL;
        return;
    }

    const httpd_uri_t get = {
        .uri = "/dns-query",
        .method = HTTP_GET,
        .handler = peer_dns_get_handler,
    };
    const httpd_uri_t post = {
        .uri = "/dns-query",
        .method = HTTP_POST,
        .handler = peer_dns_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &get);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register GET /dns-query failed: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(server, &post);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register POST /dns-query failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "exit-node DNS service on port %d", TAILSCALE_PEERAPI_PORT);
}
