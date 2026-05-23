/* Firmware OTA — manual upload from the web UI + always-on daily poll
 * of the project's GitHub Releases endpoint.
 *
 * Behaviour:
 *   - The poller runs unconditionally, once per day, plus an initial check
 *     ~20 s after boot. Polling is NOT a user-tunable setting.
 *   - When a newer release is observed, update_available flips true.
 *   - If auto_install is true, the new firmware is applied automatically:
 *       - install_hour < 0  → install as soon as the poll finds it
 *       - install_hour 0..23 → install at the next local-time wall-clock
 *                              match of that hour (so an operator can
 *                              keep reboots in their preferred quiet
 *                              window)
 *   - If auto_install is false, update_available is surfaced on the
 *     Status page as a banner; nothing reboots.
 *
 * NVS keys (namespace "tsr"):
 *   ota_auto_in  u8  — 1 if auto-install is allowed (was ota_auto_en)
 *   ota_inst_h   i8  — install hour 0..23, or -1 = "install ASAP"
 *   ota_last_check u32 — unix timestamp of the last successful poll
 *   ota_last_ver   str — version tag observed by the last poll
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

/* Load NVS settings + spawn the poller task. Idempotent. */
void      ota_init(void);

/* Stream the request body into the next OTA partition, validate, and
 * mark it bootable. Sends the JSON response itself. */
esp_err_t ota_upload_handler(httpd_req_t *req);

/* Snapshot of poller state for /api/system. All fields are best-effort
 * — if the poller hasn't run yet, the timestamps stay 0. */
typedef struct {
    bool     auto_install;         /* operator opt-in for auto-apply */
    int      install_hour;         /* 0..23 or -1 ("ASAP after poll") */
    uint32_t last_check;
    char     running_version[32];  /* esp_app_get_description()->version */
    char     last_version[32];     /* most recently observed remote tag */
    char     release_notes[768];   /* most recent release body, truncated */
    char     last_status[64];      /* "up to date", "http 404", "applied vX" … */
    bool     update_available;     /* last_version > running_version */
} ota_state_t;

void      ota_get_state(ota_state_t *out);

/* Operator-driven save. install_hour clamps to [-1, 23]. */
esp_err_t ota_set_settings(bool auto_install, int install_hour);

/* Synchronously trigger one poll right now. Returns the resulting status
 * string in `out_status` (best-effort, may be empty). */
void      ota_poll_now(char *out_status, size_t status_len);

/* Operator-driven immediate install. No-op if update_available is false.
 * Reboots on success. */
void      ota_install_now(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H_ */
