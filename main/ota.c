#include "ota.h"
#include "nvs_params.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ota";

#define OTA_REPO_OWNER   "Csontikka"
#define OTA_REPO_NAME    "esp32-tailscale-subnet-router"
#define OTA_ASSET_NAME   "firmware.bin"
#define OTA_DEFAULT_POLL_S  (6 * 3600)   /* 6 h */
#define OTA_MIN_POLL_S      900          /* 15 min — anything shorter wastes flash cycles */
#define OTA_MAX_POLL_S      (24 * 3600 * 7)
#define OTA_HTTP_RX_BUF     2048
#define OTA_API_JSON_MAX    16384        /* GitHub releases.latest JSON is ~5 KB. */

/* Cached settings + last-poll telemetry. ota_get_state reads these
 * under no lock — single-task writer (the poller task or the operator
 * via ota_set_settings) keeps races confined to single u32 fields. */
static volatile bool     s_enabled       = false;
static volatile uint32_t s_poll_s        = OTA_DEFAULT_POLL_S;
static volatile uint32_t s_last_check    = 0;
static char              s_last_version[32]  = {0};
static char              s_last_status[64]   = {0};

static TaskHandle_t      s_poll_task = NULL;
static SemaphoreHandle_t s_poll_wake = NULL;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_status, sizeof s_last_status, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_last_status);
}

/* ===== manual upload via /api/system/ota ===== */

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no OTA partition available");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA upload: %d bytes → '%s' @0x%08lx",
             req->content_len, target->label, (unsigned long)target->address);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_HTTP_RX_BUF);
    if (!buf) { esp_ota_abort(h); httpd_resp_send_500(req); return ESP_FAIL; }
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining < OTA_HTTP_RX_BUF ? remaining : OTA_HTTP_RX_BUF;
        int got  = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            esp_ota_abort(h);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(h, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                esp_err_to_name(err));
            return ESP_FAIL;
        }
        remaining -= got;
    }
    free(buf);

    err = esp_ota_end(h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            esp_err_to_name(err));
        return ESP_FAIL;
    }
    set_status("manual upload OK — %d bytes → %s", req->content_len, target->label);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"reboot_required\":true}");
}

/* ===== GitHub Releases polling ===== */

/* Drain an HTTP body into a heap buffer. Returns malloc'd NUL-terminated
 * string (caller frees) or NULL on failure / over-limit. */
static char *http_get_text(const char *url, int max_bytes, int *out_status)
{
    esp_http_client_config_t cfg = {
        .url            = url,
        .timeout_ms     = 15000,
        .buffer_size    = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* user-agent — GitHub API politely requires a non-empty UA. */
        .user_agent     = "esp32-tailscale-subnet-router-ota",
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return NULL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(cli); return NULL; }
    int total = esp_http_client_fetch_headers(cli);
    if (out_status) *out_status = esp_http_client_get_status_code(cli);

    char *buf = malloc(max_bytes + 1);
    if (!buf) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return NULL; }
    int n = 0;
    while (n < max_bytes) {
        int g = esp_http_client_read(cli, buf + n, max_bytes - n);
        if (g <= 0) break;
        n += g;
    }
    buf[n] = '\0';
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    (void)total;
    return buf;
}

/* Pluck {tag_name, download_url} from the JSON. Returns true on success
 * and writes into the caller-owned buffers. The asset URL is the
 * browser_download_url of the asset whose name matches OTA_ASSET_NAME. */
static bool parse_release_json(const char *json, char *tag, size_t tag_len,
                               char *url, size_t url_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *t = cJSON_GetObjectItem(root, "tag_name");
    if (cJSON_IsString(t) && t->valuestring) {
        strlcpy(tag, t->valuestring, tag_len);
    } else { goto out; }
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (cJSON_IsArray(assets)) {
        int n = cJSON_GetArraySize(assets);
        for (int i = 0; i < n; i++) {
            cJSON *a = cJSON_GetArrayItem(assets, i);
            if (!cJSON_IsObject(a)) continue;
            cJSON *name = cJSON_GetObjectItem(a, "name");
            cJSON *dl   = cJSON_GetObjectItem(a, "browser_download_url");
            if (cJSON_IsString(name) && cJSON_IsString(dl) &&
                strstr(name->valuestring, OTA_ASSET_NAME)) {
                strlcpy(url, dl->valuestring, url_len);
                ok = true;
                break;
            }
        }
    }
out:
    cJSON_Delete(root);
    return ok;
}

static bool version_differs(const char *remote_tag)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *local = desc ? desc->version : "";
    /* Strip a leading 'v' on either side so "v0.2.1" matches "0.2.1". */
    if (*remote_tag == 'v') remote_tag++;
    if (*local      == 'v') local++;
    return strcmp(remote_tag, local) != 0;
}

static esp_err_t do_https_ota(const char *url)
{
    esp_http_client_config_t http = {
        .url            = url,
        .timeout_ms     = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent     = "esp32-tailscale-subnet-router-ota",
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    return esp_https_ota(&ota);
}

static void poll_once(void)
{
    char url_api[160];
    snprintf(url_api, sizeof url_api,
             "https://api.github.com/repos/%s/%s/releases/latest",
             OTA_REPO_OWNER, OTA_REPO_NAME);

    int status = 0;
    char *json = http_get_text(url_api, OTA_API_JSON_MAX, &status);
    if (!json || status != 200) {
        set_status("github http %d", status);
        free(json);
        return;
    }

    char tag[32] = {0};
    char asset_url[256] = {0};
    if (!parse_release_json(json, tag, sizeof tag, asset_url, sizeof asset_url)) {
        set_status("parse failed (no %s asset?)", OTA_ASSET_NAME);
        free(json);
        return;
    }
    free(json);
    strlcpy(s_last_version, tag, sizeof s_last_version);

    if (!version_differs(tag)) {
        set_status("up to date (%s)", tag);
        return;
    }

    ESP_LOGW(TAG, "applying OTA: %s ← %s", tag, asset_url);
    esp_err_t err = do_https_ota(asset_url);
    if (err != ESP_OK) {
        set_status("ota failed: %s", esp_err_to_name(err));
        return;
    }
    set_status("ota applied %s — rebooting", tag);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void poll_task(void *arg)
{
    (void)arg;
    /* Initial settle so we don't race the network bring-up. */
    vTaskDelay(pdMS_TO_TICKS(20000));
    while (1) {
        if (s_enabled) {
            poll_once();
            time_t now = 0; time(&now);
            if (now > 1700000000) s_last_check = (uint32_t)now;
            /* Persist the snapshot so the System tab survives reboot. */
            nvs_handle_t h;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u32(h, "ota_last_check", s_last_check);
                nvs_set_str(h, "ota_last_ver",   s_last_version);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        /* Wait for either the poll interval or an explicit wake. */
        xSemaphoreTake(s_poll_wake, pdMS_TO_TICKS(s_poll_s * 1000));
    }
}

/* ===== public API ===== */

void ota_init(void)
{
    uint8_t en = 0;
    nvs_param_get_u8("ota_auto_en", &en);
    s_enabled = en != 0;

    uint32_t poll = 0;
    {
        int32_t v = 0;
        if (nvs_param_get_int("ota_poll_s", &v) == ESP_OK && v > 0) poll = (uint32_t)v;
    }
    if (poll < OTA_MIN_POLL_S) poll = OTA_DEFAULT_POLL_S;
    if (poll > OTA_MAX_POLL_S) poll = OTA_MAX_POLL_S;
    s_poll_s = poll;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "ota_last_check", (uint32_t *)&s_last_check);
        size_t l = sizeof s_last_version;
        nvs_get_str(h, "ota_last_ver", s_last_version, &l);
        nvs_close(h);
    }

    if (!s_poll_wake) s_poll_wake = xSemaphoreCreateBinary();
    if (!s_poll_task) {
        xTaskCreate(poll_task, "ota_poll", 6144, NULL, 3, &s_poll_task);
    }
    ESP_LOGI(TAG, "ota_init: enabled=%d poll_s=%lu",
             (int)s_enabled, (unsigned long)s_poll_s);
}

void ota_get_state(ota_state_t *out)
{
    if (!out) return;
    out->enabled    = s_enabled;
    out->poll_s     = s_poll_s;
    out->last_check = s_last_check;
    strlcpy(out->last_version, s_last_version, sizeof out->last_version);
    strlcpy(out->last_status,  s_last_status,  sizeof out->last_status);
}

esp_err_t ota_set_settings(bool enabled, uint32_t poll_s)
{
    if (poll_s > 0 && poll_s < OTA_MIN_POLL_S) poll_s = OTA_MIN_POLL_S;
    if (poll_s > OTA_MAX_POLL_S)               poll_s = OTA_MAX_POLL_S;
    if (poll_s == 0)                           poll_s = OTA_DEFAULT_POLL_S;
    s_enabled = enabled;
    s_poll_s  = poll_s;
    nvs_param_set_u8 ("ota_auto_en", enabled ? 1 : 0);
    nvs_param_set_int("ota_poll_s",  (int32_t)poll_s);
    /* Kick the poller so it picks up the new interval immediately. */
    if (s_poll_wake) xSemaphoreGive(s_poll_wake);
    return ESP_OK;
}

void ota_poll_now(char *out_status, size_t status_len)
{
    poll_once();
    if (out_status && status_len) strlcpy(out_status, s_last_status, status_len);
}
