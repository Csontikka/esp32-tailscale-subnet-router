/* DNS-over-HTTP endpoint used by Tailscale exit-node clients.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register GET and POST /dns-query handlers on the existing web server. */
void peer_dns_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
