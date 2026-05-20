/* Single-page web UI server.
 *
 * Boots an esp_http_server on port 80 and serves the embedded SPA at /.
 * JSON API endpoints under /api and the auth gate land in follow-up commits.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server. Idempotent — subsequent calls are no-ops. */
void web_ui_init(void);

#ifdef __cplusplus
}
#endif
