#include "ota.h"
#include "nvs_params.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ota";

#define OTA_REPO_OWNER       "Csontikka"
#define OTA_REPO_NAME        "esp32-tailscale-subnet-router"
#define OTA_ASSET_NAME       "firmware.bin"
#define OTA_HTTP_RX_BUF      2048
#define OTA_API_JSON_MAX     131072      /* 128 KB: releases/latest ~5 KB, but the beta /releases array (per_page=10)
                                          * already exceeds 32 KB (~38 KB and growing with release-notes bodies) and
                                          * was being TRUNCATED → cJSON parse-fail → "no firmware.bin asset?". Plenty
                                          * of headroom now; this buffer is a plain malloc that lands in PSRAM. */
#define OTA_RELEASES_PER_PAGE 10         /* beta channel: newest N releases to scan (newest beta is among these) */

#define OTA_POLL_INTERVAL_S  (24 * 3600) /* 1 day — non-configurable */
#define OTA_BOOT_GRACE_S     20          /* settle before the first poll */
#define OTA_TICK_S           60          /* scheduler loop tick — checks
                                          * the install_hour and re-polls
                                          * every OTA_POLL_INTERVAL_S */

/* Settings (NVS-backed) + observed-state. Strings are guarded by the
 * recursive mutex; bool/int settings stay volatile-and-atomic since they
 * fit in a single store. */
static volatile bool     s_auto_install     = false;
static volatile bool     s_beta_channel     = false; /* poll /releases (incl. pre-releases) instead of /releases/latest */
static volatile int      s_install_hour     = -1;  /* -1 = install ASAP */
static volatile uint32_t s_last_check       = 0;
static volatile bool     s_update_available = false;
static char              s_last_version[32]   = {0};
static char              s_last_status[64]    = {0};

static TaskHandle_t      s_poll_task   = NULL;
static SemaphoreHandle_t s_poll_wake   = NULL;
/* Recursive — `ota_poll_now` takes it around poll_once(), and poll_once()
 * itself calls set_status() / writes the version + notes strings inside
 * the same critical section. */
static SemaphoreHandle_t s_state_mutex = NULL;

#define WITH_STATE_LOCK(BLK) do {                                          \
    if (s_state_mutex) xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY); \
    BLK                                                                    \
    if (s_state_mutex) xSemaphoreGiveRecursive(s_state_mutex);             \
} while (0)

static void set_status(const char *fmt, ...)
{
    char tmp[sizeof s_last_status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    WITH_STATE_LOCK({
        strlcpy(s_last_status, tmp, sizeof s_last_status);
    });
    ESP_LOGI(TAG, "%s", tmp);
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
        .user_agent     = "esp32-tailscale-subnet-router-ota",
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return NULL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(cli); return NULL; }
    (void)esp_http_client_fetch_headers(cli);
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
    return buf;
}

/* Pluck {tag_name, download_url, body} from the JSON. body is truncated
 * to `notes_len-1` chars if longer (Markdown source from the release). */
static bool parse_release_json(const char *json,
                               char *tag,   size_t tag_len,
                               char *url,   size_t url_len,
                               char *notes, size_t notes_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    if (notes && notes_len) notes[0] = 0;

    cJSON *t = cJSON_GetObjectItem(root, "tag_name");
    if (cJSON_IsString(t) && t->valuestring) {
        strlcpy(tag, t->valuestring, tag_len);
    } else { goto out; }

    cJSON *body = cJSON_GetObjectItem(root, "body");
    if (notes && cJSON_IsString(body) && body->valuestring) {
        strlcpy(notes, body->valuestring, notes_len);
    }

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

/* Parsed semver: major.minor.patch + optional prerelease (everything after
 * the first '-', e.g. "beta2"). Empty prerelease = a final/stable release. */
typedef struct { int mj, mn, pt; char pre[24]; } ota_ver_t;

static void parse_version_ex(const char *s, ota_ver_t *v)
{
    v->mj = v->mn = v->pt = 0;
    v->pre[0] = '\0';
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    v->mj = atoi(s);
    const char *p = strchr(s, '.');
    if (p) { v->mn = atoi(p + 1); p = strchr(p + 1, '.'); }
    if (p) v->pt = atoi(p + 1);
    const char *dash = strchr(s, '-');           /* prerelease tag */
    if (dash && dash[1]) strlcpy(v->pre, dash + 1, sizeof v->pre);
}

/* Natural (digit-aware) compare of two prerelease strings so "beta2" < "beta10"
 * (a plain lexical compare would order them wrong). <0 a<b, 0 eq, >0 a>b. */
static int prerelease_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long na = strtol(a, (char **)&a, 10);
            long nb = strtol(b, (char **)&b, 10);
            if (na != nb) return na < nb ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++; b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* SemVer precedence (RFC §11): compare major.minor.patch, then a version
 * WITHOUT a prerelease outranks the same tuple WITH one (0.1.9 > 0.1.9-beta2),
 * and among prereleases the identifiers are compared (beta1 < beta2 < beta10).
 * <0 a<b, 0 equal, >0 a>b. */
static int version_cmp(const ota_ver_t *a, const ota_ver_t *b)
{
    if (a->mj != b->mj) return a->mj < b->mj ? -1 : 1;
    if (a->mn != b->mn) return a->mn < b->mn ? -1 : 1;
    if (a->pt != b->pt) return a->pt < b->pt ? -1 : 1;
    bool ea = !a->pre[0], eb = !b->pre[0];
    if (ea && eb) return 0;
    if (ea) return 1;                 /* stable a > prerelease b */
    if (eb) return -1;                /* prerelease a < stable b */
    return prerelease_cmp(a->pre, b->pre);
}

/* True iff `remote` is strictly newer than the running app. Equal +
 * downgrade both return false — yanked releases can't flash-wear-loop.
 * Prerelease-aware: a 0.1.9-beta1 device IS offered 0.1.9-beta2 (and later
 * the stable 0.1.9 supersedes every 0.1.9-betaN). */
static bool version_is_newer(const char *remote_tag)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    ota_ver_t r, l;
    parse_version_ex(remote_tag, &r);
    parse_version_ex(desc ? desc->version : "", &l);
    return version_cmp(&r, &l) > 0;
}

/* Cached URL for the latest-release asset, captured at poll time and
 * reused by do_install() so the scheduler doesn't have to re-poll on
 * the way to flashing. */
static char s_pending_asset_url[256] = {0};

static esp_err_t do_https_ota(const char *url)
{
    esp_http_client_config_t http = {
        .url            = url,
        .timeout_ms     = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent     = "esp32-tailscale-subnet-router-ota",
        .keep_alive_enable = true,
        /* A GitHub release-asset URL 302-redirects from github.com to a
         * *.githubusercontent.com URL with a long signed query string. The
         * default 512-byte header buffer can't hold that Location header
         * ("HTTP_CLIENT: Out of buffer" -> ESP_FAIL), so size it up. */
        .buffer_size    = 4096,
        .buffer_size_tx = 1024,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    return esp_https_ota(&ota);
}

/* Beta channel: from a /releases ARRAY, pick the highest-semver release that
 * ships a firmware.bin asset (pre-releases included; drafts skipped). */
static bool parse_newest_release_json(const char *json,
                                      char *tag, size_t tag_len,
                                      char *url, size_t url_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) { cJSON_Delete(root); return false; }
    bool ok = false;
    ota_ver_t best = {0};
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        cJSON *rel = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(rel)) continue;
        if (cJSON_IsTrue(cJSON_GetObjectItem(rel, "draft"))) continue;   /* never offer drafts */
        cJSON *t = cJSON_GetObjectItem(rel, "tag_name");
        if (!cJSON_IsString(t) || !t->valuestring) continue;
        const char *dl_url = NULL;
        cJSON *assets = cJSON_GetObjectItem(rel, "assets");
        if (cJSON_IsArray(assets)) {
            int an = cJSON_GetArraySize(assets);
            for (int a = 0; a < an; a++) {
                cJSON *as   = cJSON_GetArrayItem(assets, a);
                cJSON *name = cJSON_GetObjectItem(as, "name");
                cJSON *dl   = cJSON_GetObjectItem(as, "browser_download_url");
                if (cJSON_IsString(name) && cJSON_IsString(dl) &&
                    strstr(name->valuestring, OTA_ASSET_NAME)) { dl_url = dl->valuestring; break; }
            }
        }
        if (!dl_url) continue;   /* a release without firmware.bin can't be installed */
        ota_ver_t cur;
        parse_version_ex(t->valuestring, &cur);
        if (!ok || version_cmp(&cur, &best) > 0) {
            best = cur;
            strlcpy(tag, t->valuestring, tag_len);
            strlcpy(url, dl_url, url_len);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* Fetch the relevant release(s), parse, refresh observed-state. Does NOT
 * install — the scheduler / operator owns that decision. Stable channel uses
 * /releases/latest (GitHub excludes pre-releases); the beta channel scans
 * /releases and takes the highest-semver release, pre-releases included. */
static void poll_once(void)
{
    const bool beta = s_beta_channel;
    char url_api[200];
    if (beta) {
        snprintf(url_api, sizeof url_api,
                 "https://api.github.com/repos/%s/%s/releases?per_page=%d",
                 OTA_REPO_OWNER, OTA_REPO_NAME, OTA_RELEASES_PER_PAGE);
    } else {
        snprintf(url_api, sizeof url_api,
                 "https://api.github.com/repos/%s/%s/releases/latest",
                 OTA_REPO_OWNER, OTA_REPO_NAME);
    }

    int status = 0;
    char *json = http_get_text(url_api, OTA_API_JSON_MAX, &status);
    if (!json || status != 200) {
        set_status("github http %d", status);
        free(json);
        return;
    }

    char tag[32]        = {0};
    char asset_url[256] = {0};
    bool parsed = beta
        ? parse_newest_release_json(json, tag, sizeof tag, asset_url, sizeof asset_url)
        : parse_release_json(json, tag, sizeof tag, asset_url, sizeof asset_url, NULL, 0);
    if (!parsed) {
        set_status("parse failed (no %s asset?)", OTA_ASSET_NAME);
        free(json);
        return;
    }
    free(json);

    bool newer = version_is_newer(tag);
    WITH_STATE_LOCK({
        strlcpy(s_last_version,  tag,       sizeof s_last_version);
        strlcpy(s_pending_asset_url, asset_url, sizeof s_pending_asset_url);
        s_update_available = newer;
    });

    /* Persist the snapshot — survives reboot so the Status banner can
     * show "update available" immediately on next boot too. */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        time_t now = 0; time(&now);
        if (now > 1700000000) {
            s_last_check = (uint32_t)now;
            nvs_set_u32(h, "ota_last_check", s_last_check);
        }
        nvs_set_str(h, "ota_last_ver", s_last_version);
        nvs_commit(h);
        nvs_close(h);
    }

    set_status(newer ? "update available: %s" : "up to date (%s)", tag);
}

/* Synchronously download + flash s_pending_asset_url, then reboot.
 * Caller must hold the state mutex around the poll that primed
 * s_pending_asset_url. No-op if no pending URL. */
static void do_install(void)
{
    char url[sizeof s_pending_asset_url];
    char tag[sizeof s_last_version];
    WITH_STATE_LOCK({
        strlcpy(url, s_pending_asset_url, sizeof url);
        strlcpy(tag, s_last_version,      sizeof tag);
    });
    if (!url[0]) {
        set_status("install skipped — no asset url");
        return;
    }
    ESP_LOGW(TAG, "applying OTA: %s ← %s", tag, url);
    set_status("installing %s …", tag);
    esp_err_t err = do_https_ota(url);
    if (err != ESP_OK) {
        set_status("ota failed: %s", esp_err_to_name(err));
        return;
    }
    set_status("applied %s — rebooting", tag);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* True iff the wall-clock local-time hour matches install_hour AND we
 * haven't already attempted at this hour mark. Guarded by
 * s_last_install_attempt so multiple OTA_TICK_S ticks inside the same
 * hour don't re-fire. */
static uint32_t s_last_install_attempt = 0;

static bool is_install_window_now(int hour_wanted)
{
    if (hour_wanted < 0 || hour_wanted > 23) return false;
    time_t now_t = 0; time(&now_t);
    if (now_t < 1700000000) return false;   /* SNTP not synced yet */
    struct tm lt;
    localtime_r(&now_t, &lt);
    if (lt.tm_hour != hour_wanted) return false;
    if ((uint32_t)now_t - s_last_install_attempt < 3600) return false;
    s_last_install_attempt = (uint32_t)now_t;
    return true;
}

static void poll_task(void *arg)
{
    (void)arg;
    /* Initial settle so we don't race the network bring-up. */
    vTaskDelay(pdMS_TO_TICKS(OTA_BOOT_GRACE_S * 1000));

    uint32_t last_poll_uptime_s = 0;
    bool first = true;

    while (1) {
        uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

        /* 1. Periodic GitHub check — first iteration + every 24 h after. */
        if (first || (up_s - last_poll_uptime_s) >= OTA_POLL_INTERVAL_S) {
            poll_once();
            last_poll_uptime_s = up_s;
            first = false;
        }

        /* 2. Install scheduler — only acts when auto-install is on
         *    AND a newer version is available. */
        if (s_auto_install && s_update_available) {
            if (s_install_hour < 0) {
                do_install();   /* "ASAP" mode — reboots if successful */
            } else if (is_install_window_now(s_install_hour)) {
                do_install();
            }
        }

        /* Sleep for OTA_TICK_S or until a setting change / manual wake. */
        TickType_t tick = (TickType_t)((uint64_t)OTA_TICK_S * 1000ULL /
                                       (uint64_t)portTICK_PERIOD_MS);
        xSemaphoreTake(s_poll_wake, tick);
    }
}

/* ===== public API ===== */

void ota_init(void)
{
    /* New key `ota_auto_in` (install opt-in). Fall back to the legacy
     * `ota_auto_en` so devices upgrading across this commit don't lose
     * their preference. */
    uint8_t v8 = 0;
    if (nvs_param_get_u8("ota_auto_in", &v8) != ESP_OK) {
        (void)nvs_param_get_u8("ota_auto_en", &v8);
    }
    s_auto_install = v8 != 0;

    int32_t hour = -1;
    if (nvs_param_get_int("ota_inst_h", &hour) == ESP_OK) {
        if (hour < -1 || hour > 23) hour = -1;
    } else {
        hour = -1;
    }
    s_install_hour = (int)hour;

    uint8_t beta8 = 0;
    (void)nvs_param_get_u8("ota_beta", &beta8);
    s_beta_channel = beta8 != 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "ota_last_check", (uint32_t *)&s_last_check);
        size_t l = sizeof s_last_version;
        nvs_get_str(h, "ota_last_ver", s_last_version, &l);
        nvs_close(h);
    }
    /* Compute update_available from the persisted last_version vs the
     * current running version, so the Status banner is correct even
     * before the first post-boot poll completes. */
    if (s_last_version[0]) s_update_available = version_is_newer(s_last_version);

    if (!s_state_mutex) s_state_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_poll_wake)   s_poll_wake   = xSemaphoreCreateBinary();
    if (!s_poll_task) {
        xTaskCreate(poll_task, "ota_poll", 6144, NULL, 3, &s_poll_task);
    }
    ESP_LOGI(TAG, "ota_init: auto_install=%d install_hour=%d beta=%d update_avail=%d",
             (int)s_auto_install, s_install_hour, (int)s_beta_channel, (int)s_update_available);
}

void ota_get_state(ota_state_t *out)
{
    if (!out) return;
    out->auto_install      = s_auto_install;
    out->beta_channel      = s_beta_channel;
    out->install_hour      = s_install_hour;
    out->last_check        = s_last_check;
    out->update_available  = s_update_available;
    const esp_app_desc_t *desc = esp_app_get_description();
    strlcpy(out->running_version, desc ? desc->version : "",
            sizeof out->running_version);
    WITH_STATE_LOCK({
        strlcpy(out->last_version,   s_last_version,   sizeof out->last_version);
        strlcpy(out->last_status,    s_last_status,    sizeof out->last_status);
    });
}

esp_err_t ota_set_settings(bool auto_install, int install_hour)
{
    if (install_hour < -1 || install_hour > 23) install_hour = -1;
    s_auto_install = auto_install;
    s_install_hour = install_hour;
    nvs_param_set_u8 ("ota_auto_in", auto_install ? 1 : 0);
    nvs_param_set_int("ota_inst_h",  (int32_t)install_hour);
    /* Wake the scheduler so a newly-enabled auto_install + ASAP can
     * react without waiting OTA_TICK_S. */
    if (s_poll_wake) xSemaphoreGive(s_poll_wake);
    return ESP_OK;
}

/* Beta channel opt-in. When on, the poller scans /releases (pre-releases
 * included) instead of /releases/latest. Re-poll explicitly afterwards
 * (ota_poll_now) to apply the channel switch immediately. */
esp_err_t ota_set_beta(bool enabled)
{
    s_beta_channel = enabled;
    nvs_param_set_u8("ota_beta", enabled ? 1 : 0);
    return ESP_OK;
}

void ota_poll_now(char *out_status, size_t status_len)
{
    /* Hold the mutex across the whole poll so the background poll_task
     * can't race us mid-poll. set_status() and the strlcpy()s inside
     * poll_once() each re-enter the same mutex; recursive semantics keep
     * that safe. The HTTP fetch is slow (~3 s) so the held window is
     * real, but ota_poll_now is an explicit operator click. */
    WITH_STATE_LOCK({
        poll_once();
        if (out_status && status_len) {
            strlcpy(out_status, s_last_status, status_len);
        }
    });
}

void ota_install_now(void)
{
    if (!s_update_available) {
        set_status("install skipped — no newer version known");
        return;
    }
    do_install();
}
