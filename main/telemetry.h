/* Anonymous telemetry — privacy-respecting flash/boot/heartbeat reporter.
 *
 * What it does:
 *   On boot, increments tm_boot_cnt. If the SHA-mismatch flash detector
 *   in esp32_nat_router.c set last_rst="FLASH" then it also bumps
 *   tm_flash_cnt. After WiFi STA is up and a 60 s grace period has
 *   passed, a low-priority task POSTs a JSON event to a Cloudflare
 *   Worker:
 *
 *     POST <url>
 *     X-Tlm-Key: <key>
 *     Content-Type: application/json
 *     { dh, v, et, bc, fc, up, rr, ch, fh, ac, ts }
 *
 *   "flash" + "boot" the first time, then "heartbeat" every 24 h.
 *
 * What it does NOT do:
 *   - Send the raw MAC. Only SHA-256(MAC || salt)[0..7] in hex.
 *   - Send WiFi SSID, IP addresses, or any client/peer identifying info.
 *   - Send anything if tm_enabled == 0 (opt-out from /config).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELEMETRY_DEFAULT_URL "https://esp32-tailscale-telemetry.csontikka.workers.dev/v1/event"
#define TELEMETRY_DEFAULT_KEY "9f8f78c783d95fd53d14b7466bcbc9e2"

#define TELEMETRY_URL_MAXLEN   160
#define TELEMETRY_KEY_MAXLEN    48
#define TELEMETRY_BAN_VER_MAX   40
#define TELEMETRY_STATUS_MAX    64
#define TELEMETRY_DH_HEX_LEN    16

typedef struct {
    bool      enabled;
    char      url[TELEMETRY_URL_MAXLEN];
    char      key[TELEMETRY_KEY_MAXLEN];
    uint32_t  boot_count;
    uint32_t  flash_count;
    char      device_hash[TELEMETRY_DH_HEX_LEN + 1];
    uint64_t  last_send_ms;                       /* 0 = never sent */
    char      last_status[TELEMETRY_STATUS_MAX];  /* short human-readable */
    char      banner_seen_ver[TELEMETRY_BAN_VER_MAX];
} telemetry_state_t;

/* Initialise: load NVS, compute device_hash, increment counters, spawn the
 * sender task. Safe to call before WiFi is up — the task waits for STA.
 * Idempotent (subsequent calls are no-ops). */
void telemetry_init(void);

/* UI getter — returns a copy. */
telemetry_state_t telemetry_get_state(void);

/* Setters — persist to NVS + apply immediately. */
esp_err_t telemetry_set_enabled(bool enabled);
esp_err_t telemetry_set_url(const char *url);
esp_err_t telemetry_set_key(const char *key);

/* Index-page banner control. The banner is shown once per firmware
 * version; dismissing it stamps the current version into NVS so it
 * stays away until the next upgrade. */
void telemetry_dismiss_banner(void);
bool telemetry_should_show_banner(void);

/* Trigger an immediate "heartbeat" send (UI Test button). Async — returns
 * before the HTTP request completes. */
void telemetry_send_now(void);

#ifdef __cplusplus
}
#endif
