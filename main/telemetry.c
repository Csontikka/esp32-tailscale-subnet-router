/* Anonymous telemetry — see include/telemetry.h for the design rationale.
 *
 * Lifecycle (called from app_main after WiFi/tailscale init):
 *   1. compute_device_hash()       — SHA-256(MAC || salt)[0..7]
 *   2. load_nvs()                  — pulls persisted state
 *   3. detect FLASH event          — reads last_rst written by the
 *                                    SHA-mismatch flash detector in
 *                                    esp32_nat_router.c
 *   4. spawn sender_task           — waits for ap_connect, +60 s grace,
 *                                    sends boot (+flash if applicable),
 *                                    then 24 h heartbeat loop
 *
 * The sender task uses esp_http_client with crt_bundle_attach so the
 * Cloudflare TLS cert validates against the IDF bundled CA list.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM stack */
#include "freertos/semphr.h"
#include "esp_heap_caps.h"            /* MALLOC_CAP_SPIRAM */

#include "nvs_params.h"
#include "tailscale_config.h"
#include "telemetry.h"

/* Cross-module state owned by main.c. ap_connect lets the sender task
 * gate the first send on the upstream STA being up; connect_count is
 * the AP-client count we report in every event. */
extern int ap_connect;
extern int connect_count;

static const char *TAG = "telemetry";

#define TLM_NVS_ENABLED      "tm_enabled"
#define TLM_NVS_URL          "tm_url"
#define TLM_NVS_KEY          "tm_key"
#define TLM_NVS_BOOT_CNT     "tm_boot_cnt"
#define TLM_NVS_FLASH_CNT    "tm_flash_cnt"
#define TLM_NVS_BAN_VER      "tm_ban_ver"

/* 180 s grace gives microlink time to finish its initial DERP+STUN
 * handshakes — those allocate transient mbedTLS buffers that otherwise
 * collide with our own SSL setup and cause MBEDTLS_ERR_SSL_ALLOC_FAILED
 * (observed at uptime 65 s on the first deploy). */
#define TLM_FIRST_SEND_DELAY_S   180u
#define TLM_HEARTBEAT_PERIOD_S   86400u

/* Retry schedule (seconds) on transient HTTP/TLS failure. After the last
 * entry we fall back to the regular 24 h heartbeat. */
static const uint32_t TLM_RETRY_BACKOFF_S[] = {30, 60, 120, 300, 600};
#define TLM_RETRY_BACKOFF_N  (sizeof(TLM_RETRY_BACKOFF_S)/sizeof(TLM_RETRY_BACKOFF_S[0]))

static telemetry_state_t s_state;
static SemaphoreHandle_t s_sem = NULL;
static TaskHandle_t      s_task = NULL;
static bool              s_just_flashed = false;
static char              s_chip_buf[16];

static void set_defaults(void)
{
    s_state.enabled = true;
    strncpy(s_state.url, TELEMETRY_DEFAULT_URL, sizeof(s_state.url) - 1);
    s_state.url[sizeof(s_state.url) - 1] = 0;
    strncpy(s_state.key, TELEMETRY_DEFAULT_KEY, sizeof(s_state.key) - 1);
    s_state.key[sizeof(s_state.key) - 1] = 0;
    s_state.boot_count = 0;
    s_state.flash_count = 0;
    s_state.device_hash[0] = 0;
    s_state.last_send_ms = 0;
    strncpy(s_state.last_status, "not yet sent", sizeof(s_state.last_status) - 1);
    s_state.banner_seen_ver[0] = 0;
}

static void compute_device_hash(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    static const char salt[] = "esp32-tailscale-router-v1";
    uint8_t input[6 + sizeof(salt) - 1];
    memcpy(input, mac, 6);
    memcpy(input + 6, salt, sizeof(salt) - 1);

    uint8_t digest[32];
    mbedtls_sha256(input, sizeof(input), digest, 0);

    for (int i = 0; i < 8; i++) {
        snprintf(s_state.device_hash + i * 2, 3, "%02x", digest[i]);
    }
    s_state.device_hash[TELEMETRY_DH_HEX_LEN] = 0;
}

static const char *chip_model_str(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char *m;
    switch (info.model) {
        case CHIP_ESP32:    m = "ESP32"; break;
        case CHIP_ESP32S2:  m = "S2";    break;
        case CHIP_ESP32S3:  m = "S3";    break;
        case CHIP_ESP32C3:  m = "C3";    break;
        case CHIP_ESP32C6:  m = "C6";    break;
        case CHIP_ESP32H2:  m = "H2";    break;
        default:            m = "?";     break;
    }
    snprintf(s_chip_buf, sizeof(s_chip_buf), "%sr%d", m, info.revision);
    return s_chip_buf;
}

static int reset_reason_code(void)
{
    /* Custom codes:
     *   100 = FLASH    (firmware bytes changed since last boot)
     *   101 = ROLLBACK (bootloader fell back to previous slot)
     * Otherwise the raw ESP-IDF esp_reset_reason_t value. */
    char rs[24] = "";
    size_t l = sizeof(rs);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "last_rst", rs, &l);
        nvs_close(h);
    }
    if (strcmp(rs, "FLASH") == 0)    return 100;
    if (strcmp(rs, "ROLLBACK") == 0) return 101;
    return (int)esp_reset_reason();
}

/* True if this boot's last_rst indicates the previous run ended with a
 * crash (PANIC / WDT). Used to gate the crash-signature inclusion in
 * telemetry — we want it sent once, on the first boot after the crash. */
static bool last_rst_is_crash(void)
{
    char rs[24] = "";
    size_t l = sizeof(rs);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_str(h, "last_rst", rs, &l);
    nvs_close(h);
    return (strcmp(rs, "PANIC")    == 0) ||
           (strcmp(rs, "INT_WDT")  == 0) ||
           (strcmp(rs, "TASK_WDT") == 0) ||
           (strcmp(rs, "WDT")      == 0) ||
           (strcmp(rs, "BROWNOUT") == 0);
}

/* Reads NVS.last_crash (set by esp32_nat_router.c from the coredump
 * summary). Format: "task=NAME pc=0xADDR bt=0xADDR,0xADDR,0xADDR" — only
 * function pointers + task name, no stack content. Returns "" if empty. */
static void read_crash_sig(char *out, size_t out_sz)
{
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t l = out_sz;
    nvs_get_str(h, "last_crash", out, &l);
    nvs_close(h);
}

static const char *tailscale_status_str(void)
{
    return tailscale_enabled ? "up" : "off";
}

static void set_status(const char *s)
{
    strncpy(s_state.last_status, s, sizeof(s_state.last_status) - 1);
    s_state.last_status[sizeof(s_state.last_status) - 1] = 0;
}

static esp_err_t do_send(const char *event_type)
{
    if (!s_state.enabled) return ESP_ERR_INVALID_STATE;
    if (s_state.url[0] == 0) {
        set_status("disabled (no URL)");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = desc ? desc->version : "unknown";
    /* Build date/time in compact "V<YY>.<MMDD>.<HHMM>" form — every
     * compile refreshes __DATE__/__TIME__ (see scripts/
     * force_rebuild_app_desc.py). The semver-style version barely
     * changes; this gives per-flash visibility in the dashboard. */
    char build_buf[32];
    build_buf[0] = 0;
    if (desc) {
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = {0}, year[5] = {0};
        int day = 0, hour = 0, minute = 0;
        if (sscanf(desc->date, "%3s %d %4s", mon, &day, year) == 3 &&
            sscanf(desc->time, "%d:%d", &hour, &minute) == 2) {
            const char *p = strstr(months, mon);
            unsigned mnum   = p ? (unsigned)(((p - months) / 3) + 1) : 0u;
            unsigned uday   = (day    > 0 && day    < 100) ? (unsigned)day    : 0u;
            unsigned uhour  = (hour   >= 0 && hour   < 100) ? (unsigned)hour   : 0u;
            unsigned umin   = (minute >= 0 && minute < 100) ? (unsigned)minute : 0u;
            snprintf(build_buf, sizeof(build_buf),
                     "V%c%c.%02u%02u.%02u%02u",
                     year[2], year[3], mnum, uday, uhour, umin);
        }
    }
    const char *build = build_buf;

    uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    uint32_t free_heap = esp_get_free_heap_size();

    /* Only attach a crash signature on the first boot after PANIC/WDT.
     * The signature is "task=NAME pc=0xADDR bt=0x...,0x...,0x..." — code
     * addresses only, no stack content (see read_crash_sig comment). */
    char crash_sig[180] = "";
    if (last_rst_is_crash()) {
        read_crash_sig(crash_sig, sizeof(crash_sig));
    }

    /* Reboot cause beyond the coarse rr code (e.g. "ch-realign 11->1"),
     * set in main.c at boot from NVS reboot_why; "" for untagged resets.
     * Sent as "rw" so the dashboard can distinguish a deliberate channel
     * realign from a generic SW reset. */
    extern char g_reboot_why[];

    char body[640];
    int n;
    if (crash_sig[0]) {
        n = snprintf(body, sizeof(body),
            "{"
            "\"dh\":\"%s\","
            "\"v\":\"%s\","
            "\"bd\":\"%s\","
            "\"et\":\"%s\","
            "\"bc\":%u,"
            "\"fc\":%u,"
            "\"up\":%llu,"
            "\"rr\":%d,"
            "\"rw\":\"%s\","
            "\"ch\":\"%s\","
            "\"fh\":%u,"
            "\"ac\":%d,"
            "\"ts\":\"%s\","
            "\"cr\":\"%s\""
            "}",
            s_state.device_hash, ver, build, event_type,
            (unsigned)s_state.boot_count, (unsigned)s_state.flash_count,
            (unsigned long long)uptime_s, reset_reason_code(), g_reboot_why,
            chip_model_str(), (unsigned)free_heap, connect_count,
            tailscale_status_str(), crash_sig);
    } else {
        n = snprintf(body, sizeof(body),
            "{"
            "\"dh\":\"%s\","
            "\"v\":\"%s\","
            "\"bd\":\"%s\","
            "\"et\":\"%s\","
            "\"bc\":%u,"
            "\"fc\":%u,"
            "\"up\":%llu,"
            "\"rr\":%d,"
            "\"rw\":\"%s\","
            "\"ch\":\"%s\","
            "\"fh\":%u,"
            "\"ac\":%d,"
            "\"ts\":\"%s\""
            "}",
            s_state.device_hash, ver, build, event_type,
            (unsigned)s_state.boot_count, (unsigned)s_state.flash_count,
            (unsigned long long)uptime_s, reset_reason_code(), g_reboot_why,
            chip_model_str(), (unsigned)free_heap, connect_count,
            tailscale_status_str());
    }
    if (n < 0 || n >= (int)sizeof(body)) {
        set_status("body too large");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url               = s_state.url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_status("client init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Tlm-Key", s_state.key);
    esp_http_client_set_post_field(client, body, n);

    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        char buf[TELEMETRY_STATUS_MAX];
        snprintf(buf, sizeof(buf), "send err: %s", esp_err_to_name(err));
        set_status(buf);
        ESP_LOGW(TAG, "send %s failed: %s", event_type, esp_err_to_name(err));
        return err;
    }
    if (code < 200 || code >= 300) {
        char buf[TELEMETRY_STATUS_MAX];
        snprintf(buf, sizeof(buf), "HTTP %d", code);
        set_status(buf);
        ESP_LOGW(TAG, "send %s -> HTTP %d", event_type, code);
        return ESP_FAIL;
    }

    char buf[TELEMETRY_STATUS_MAX];
    snprintf(buf, sizeof(buf), "%s OK", event_type);
    set_status(buf);
    s_state.last_send_ms = esp_timer_get_time() / 1000ULL;
    ESP_LOGI(TAG, "send %s -> HTTP 200", event_type);
    return ESP_OK;
}

/* Retries the POST on transient TLS/HTTP errors. Returns ESP_OK on the
 * first success, or the last error after all backoff attempts have run. */
static esp_err_t do_send_with_retry(const char *event_type)
{
    esp_err_t err = do_send(event_type);
    if (err == ESP_OK) return ESP_OK;

    for (size_t i = 0; i < TLM_RETRY_BACKOFF_N; i++) {
        uint32_t delay_s = TLM_RETRY_BACKOFF_S[i];
        ESP_LOGW(TAG, "%s retry %u/%u in %us (free heap %u)",
                 event_type, (unsigned)(i + 1), (unsigned)TLM_RETRY_BACKOFF_N,
                 (unsigned)delay_s, (unsigned)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        err = do_send(event_type);
        if (err == ESP_OK) return ESP_OK;
    }
    return err;
}

static void sender_task(void *arg)
{
    (void)arg;

    /* Wait for first STA association. ap_connect is a global int set
     * by the WiFi event handlers in esp32_nat_router.c. */
    while (!ap_connect) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelay(pdMS_TO_TICKS(TLM_FIRST_SEND_DELAY_S * 1000));

    bool first_iter = true;
    while (1) {
        if (s_state.enabled) {
            ESP_LOGI(TAG, "send tick (free heap %u, first=%d, flashed=%d)",
                     (unsigned)esp_get_free_heap_size(),
                     (int)first_iter, (int)s_just_flashed);
            if (first_iter && s_just_flashed) {
                do_send_with_retry("flash");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            do_send_with_retry(first_iter ? "boot" : "heartbeat");
            first_iter = false;
        }
        /* Sleep for 24 h or until the semaphore is given (URL change,
         * Test button, opt-in toggle).
         *
         * NOTE: do NOT use pdMS_TO_TICKS(86_400_000) here. In the
         * non-SMP FreeRTOS-Kernel build (the ESP-IDF default), the
         * macro is `(TickType_t)xTimeInMs * (TickType_t)TICK_RATE_HZ
         * / 1000U` — both operands cast to uint32_t before the
         * multiply, so 86_400_000 * 100 overflows to 50_065_408,
         * yielding ~50_065 ticks ≈ 500 s ≈ 8m21s instead of 24 h.
         * The bug surfaced as a heartbeat-every-8-minutes flood on
         * the worker. Compute in uint64_t to keep the result correct. */
        TickType_t heartbeat_ticks =
            (TickType_t)((uint64_t)TLM_HEARTBEAT_PERIOD_S * 1000ULL /
                         (uint64_t)portTICK_PERIOD_MS);
        xSemaphoreTake(s_sem, heartbeat_ticks);
    }
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t en = 1;
    if (nvs_get_u8(h, TLM_NVS_ENABLED, &en) == ESP_OK) {
        s_state.enabled = (en != 0);
    }
    size_t l = sizeof(s_state.url);
    nvs_get_str(h, TLM_NVS_URL, s_state.url, &l);
    l = sizeof(s_state.key);
    nvs_get_str(h, TLM_NVS_KEY, s_state.key, &l);

    uint32_t bc = 0, fc = 0;
    nvs_get_u32(h, TLM_NVS_BOOT_CNT, &bc);
    nvs_get_u32(h, TLM_NVS_FLASH_CNT, &fc);
    s_state.boot_count = bc;
    s_state.flash_count = fc;

    l = sizeof(s_state.banner_seen_ver);
    nvs_get_str(h, TLM_NVS_BAN_VER, s_state.banner_seen_ver, &l);

    nvs_close(h);
}

static void save_counters(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, TLM_NVS_BOOT_CNT, s_state.boot_count);
    nvs_set_u32(h, TLM_NVS_FLASH_CNT, s_state.flash_count);
    nvs_commit(h);
    nvs_close(h);
}

static bool detect_just_flashed_from_nvs(void)
{
    char rs[24] = "";
    size_t l = sizeof(rs);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_str(h, "last_rst", rs, &l);
    nvs_close(h);
    return strcmp(rs, "FLASH") == 0;
}

void telemetry_init(void)
{
    if (s_task) return;

    set_defaults();
    compute_device_hash();
    load_nvs();

    s_just_flashed = detect_just_flashed_from_nvs();
    s_state.boot_count++;
    if (s_just_flashed) s_state.flash_count++;
    save_counters();

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "init: dh=%s ver=%s boot=%u flash=%u flashed=%d enabled=%d",
             s_state.device_hash,
             desc ? desc->version : "?",
             (unsigned)s_state.boot_count,
             (unsigned)s_state.flash_count,
             (int)s_just_flashed,
             (int)s_state.enabled);

    if (!s_sem) s_sem = xSemaphoreCreateBinary();

    /* 8 KB stack: mbedtls TLS handshake allocates a few KB transiently. */
    /* Stack in PSRAM (TCB stays internal) — sender_task only does HTTPS/NVS-read,
     * never SPI-flash writes; XIP keeps the cache live during flash ops. ~8K. */
    xTaskCreateWithCaps(sender_task, "telemetry", 8192, NULL, 3, &s_task, MALLOC_CAP_SPIRAM);
}

telemetry_state_t telemetry_get_state(void)
{
    return s_state;
}

esp_err_t telemetry_set_enabled(bool enabled)
{
    bool was_enabled = s_state.enabled;
    s_state.enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, TLM_NVS_ENABLED, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    /* Wake sender ONLY on a true OFF->ON transition. Re-saving the
     * /config form with the radio unchanged was firing the semaphore
     * every click, producing spurious heartbeats. */
    if (enabled && !was_enabled && s_sem) xSemaphoreGive(s_sem);
    return ESP_OK;
}

esp_err_t telemetry_set_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    strncpy(s_state.url, url, sizeof(s_state.url) - 1);
    s_state.url[sizeof(s_state.url) - 1] = 0;
    return nvs_param_set_str(TLM_NVS_URL, s_state.url);
}

esp_err_t telemetry_set_key(const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    strncpy(s_state.key, key, sizeof(s_state.key) - 1);
    s_state.key[sizeof(s_state.key) - 1] = 0;
    return nvs_param_set_str(TLM_NVS_KEY, s_state.key);
}

void telemetry_dismiss_banner(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) return;
    strncpy(s_state.banner_seen_ver, desc->version,
            sizeof(s_state.banner_seen_ver) - 1);
    s_state.banner_seen_ver[sizeof(s_state.banner_seen_ver) - 1] = 0;
    nvs_param_set_str(TLM_NVS_BAN_VER, s_state.banner_seen_ver);
}

bool telemetry_should_show_banner(void)
{
    if (!s_state.enabled) return false;
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) return false;
    return strcmp(desc->version, s_state.banner_seen_ver) != 0;
}

void telemetry_send_now(void)
{
    if (s_sem) xSemaphoreGive(s_sem);
}
