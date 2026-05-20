/* Synchronous ICMP echo / traceroute helper for the /diag GUI page.
 *
 * Single-shot blocking calls suited for HTTP request handlers: returns the
 * full output as plain-text lines into the caller-supplied buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run `count` ICMP echo requests against `target` (hostname or dotted IPv4).
 * Output is appended (newline-separated) to `out`. Returns 0 on success
 * (any reply received), non-zero on hard errors (DNS, socket). */
int net_diag_ping(const char *target, int count, int timeout_ms,
                  char *out, size_t out_size);

/* Run a traceroute against `target`: TTL=1..max_hops, one probe per hop.
 * Stops at first echo-reply (destination reached) or after max_hops.
 * Output format: one line per hop. */
int net_diag_trace(const char *target, int max_hops, int timeout_ms,
                   char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
