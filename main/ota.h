/* Firmware OTA — manual upload from the web UI + optional periodic
 * poll of the project's GitHub Releases endpoint.
 *
 * The HTTPD handler for POST /api/system/ota is registered by web_ui.c
 * (so it shares the auth + httpd_uri_t plumbing); this module owns the
 * actual OTA state machine plus the GitHub poller task.
 *
 * NVS keys (namespace "tsr"):
 *   ota_auto_en   u8  — 1 if the GitHub poller should run
 *   ota_poll_s    u32 — poll interval; 0 = use default (6 hours)
 *   ota_last_check u32 — unix timestamp of the last successful poll
 *   ota_last_ver   str — version tag observed by the last poll (e.g. "v0.2.1")
 *
 * SPDX-License-Identifier: MIT */

#ifndef OTA_H_
#define OTA_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load NVS settings + spawn the poller task (only when enabled).
 * Idempotent. */
void      ota_init(void);

/* Stream the request body into the next OTA partition, validate, and
 * mark it bootable. Sends the JSON response itself. */
esp_err_t ota_upload_handler(httpd_req_t *req);

/* Snapshot of poller state for /api/system. All fields are best-effort
 * — if the poller hasn't run yet, the timestamps stay 0. */
typedef struct {
    bool     enabled;
    uint32_t poll_s;
    uint32_t last_check;
    char     last_version[32];
    char     last_status[64];  /* e.g. "no update", "applied v0.2.1", "http 503" */
} ota_state_t;

void      ota_get_state(ota_state_t *out);

/* Operator-driven save of the poller settings. enabled toggles the task;
 * poll_s clamps to a sensible window. */
esp_err_t ota_set_settings(bool enabled, uint32_t poll_s);

/* Synchronously trigger one poll right now. Returns the resulting status
 * string in `out_status` (best-effort, may be empty). */
void      ota_poll_now(char *out_status, size_t status_len);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H_ */
