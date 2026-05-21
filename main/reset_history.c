#include "reset_history.h"
#include "nvs_params.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "nvs.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rst_hist";

#define KEY_BLOB   "rst_hist"
#define KEY_LAST   "last_rst"
#define KEY_COUNT  "rst_count"
#define KEY_WHO    "rst_who"
#define KEY_APPSHA "app_sha8"
#define KEY_CRASH  "last_crash"

/* The legacy 84-byte-per-entry blob shape from before we added the
 * `crash[160]` field. Used only by the v1→v2 migration path. */
typedef struct {
    uint32_t wallclock;
    char     reason[16];
    char     who[64];
} old_entry_t;

/* Sanitise every string field's NUL terminator, and clear the crash
 * column on rows whose reason isn't panic-shaped — defensive cleanup of
 * any leakage from earlier code paths that didn't always zero hist[0]
 * before writing a fresh non-crash row. */
static void sanitize_blob(reset_history_entry_t *hist)
{
    for (int i = 0; i < RESET_HISTORY_MAX; i++) {
        hist[i].reason[sizeof(hist[i].reason) - 1] = '\0';
        hist[i].who   [sizeof(hist[i].who)    - 1] = '\0';
        hist[i].crash [sizeof(hist[i].crash)  - 1] = '\0';
        if (hist[i].reason[0] == '\0') continue;
        bool is_crash_row = (strstr(hist[i].reason, "PANIC") != NULL)
                         || (strstr(hist[i].reason, "WDT")   != NULL);
        if (!is_crash_row) {
            memset(hist[i].crash, 0, sizeof(hist[i].crash));
        }
    }
}

/* Migrate a legacy v1 blob (no crash field) into the in-memory v2 layout,
 * then best-effort back-fill the newest PANIC/WDT row with NVS.last_crash
 * so the recovered history isn't blank for the crash that triggered the
 * firmware update. */
static void migrate_v1_to_v2(nvs_handle_t h, reset_history_entry_t *hist)
{
    old_entry_t old_hist[RESET_HISTORY_MAX] = {0};
    size_t old_sz = sizeof(old_hist);
    if (nvs_get_blob(h, KEY_BLOB, old_hist, &old_sz) != ESP_OK) return;
    for (int i = 0; i < RESET_HISTORY_MAX; i++) {
        hist[i].wallclock = old_hist[i].wallclock;
        strncpy(hist[i].reason, old_hist[i].reason, sizeof(hist[i].reason) - 1);
        strncpy(hist[i].who,    old_hist[i].who,    sizeof(hist[i].who)    - 1);
        hist[i].crash[0] = '\0';
    }
    char lc[160] = "";
    size_t lc_len = sizeof(lc);
    if (nvs_get_str(h, KEY_CRASH, lc, &lc_len) == ESP_OK && lc[0]) {
        for (int i = 0; i < RESET_HISTORY_MAX; i++) {
            if (strstr(hist[i].reason, "PANIC") ||
                strstr(hist[i].reason, "WDT")) {
                strncpy(hist[i].crash, lc, sizeof(hist[i].crash) - 1);
                break;
            }
        }
    }
    ESP_LOGW(TAG, "rst_hist v1 -> v2 migrated, last_crash back-filled");
}

/* Compute a human label for the current boot's reset cause. The
 * FLASH/ROLLBACK detection sits in front of esp_reset_reason() because
 * IDF v5.3 reports UNKNOWN for both first-boot and the post-flash chip
 * reset, and we want those distinguished — the operator cares.
 *
 * `out_who` receives the deliberate-restart attribution (from NVS
 * KEY_WHO), but only when the reset reason was SW — otherwise that key
 * is stale leftover from some prior esp_restart() and would misattribute
 * the current boot. Caller clears stale KEY_WHO regardless. */
static const char *classify_reset(nvs_handle_t h, char *out_who, size_t who_len)
{
    bool just_flashed = false;
    bool rolled_back  = false;
    const esp_app_desc_t *desc = esp_app_get_description();
    char cur_sha_hex[17] = "";
    if (desc) {
        for (int i = 0; i < 8; i++) {
            snprintf(cur_sha_hex + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
        }
    }
    char saved_sha[17] = "";
    size_t l = sizeof(saved_sha);
    nvs_get_str(h, KEY_APPSHA, saved_sha, &l);
    if (cur_sha_hex[0] && strcmp(saved_sha, cur_sha_hex) != 0) {
        if (esp_ota_get_last_invalid_partition() != NULL) {
            rolled_back = true;
        } else {
            just_flashed = true;
        }
        nvs_set_str(h, KEY_APPSHA, cur_sha_hex);
    }

    esp_reset_reason_t rr = esp_reset_reason();
    const char *rs;
    if (rolled_back)      rs = "ROLLBACK";
    else if (just_flashed) rs = "FLASH";
    else switch (rr) {
        case ESP_RST_POWERON:   rs = "POWERON";   break;
        case ESP_RST_EXT:       rs = "EXT";       break;
        case ESP_RST_SW:        rs = "SW";        break;
        case ESP_RST_PANIC:     rs = "PANIC";     break;
        case ESP_RST_INT_WDT:   rs = "INT_WDT";   break;
        case ESP_RST_TASK_WDT:  rs = "TASK_WDT";  break;
        case ESP_RST_WDT:       rs = "WDT";       break;
        case ESP_RST_DEEPSLEEP: rs = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT:  rs = "BROWNOUT";  break;
        case ESP_RST_SDIO:      rs = "SDIO";      break;
        default:                rs = "UNKNOWN";   break;
    }

    if (out_who && who_len) out_who[0] = '\0';
    if (rr == ESP_RST_SW && out_who && who_len) {
        size_t wl = who_len;
        nvs_get_str(h, KEY_WHO, out_who, &wl);
    }
    if (rr != ESP_RST_SW) {
        nvs_erase_key(h, KEY_WHO);
    }

    nvs_set_str(h, KEY_LAST,  rs);
    nvs_set_i32(h, KEY_COUNT, (int32_t)rr);
    return rs;
}

void reset_history_record_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed — skipping reset history", NVS_NAMESPACE);
        return;
    }

    char who_capture[64] = "";
    const char *rs = classify_reset(h, who_capture, sizeof(who_capture));
    ESP_LOGW(TAG, "=== reset reason: %s%s%s ===",
             rs, who_capture[0] ? " who=" : "", who_capture[0] ? who_capture : "");

    reset_history_entry_t hist[RESET_HISTORY_MAX] = {0};
    size_t blob_size = sizeof(hist);
    esp_err_t e = nvs_get_blob(h, KEY_BLOB, hist, &blob_size);
    if (e == ESP_ERR_NVS_INVALID_LENGTH) {
        size_t legacy_sz = sizeof(old_entry_t) * RESET_HISTORY_MAX;
        size_t found = 0;
        nvs_get_blob(h, KEY_BLOB, NULL, &found);
        if (found == legacy_sz) {
            memset(hist, 0, sizeof(hist));
            migrate_v1_to_v2(h, hist);
        }
    }
    sanitize_blob(hist);

    /* Shift everything one slot down, drop the oldest entry. */
    memmove(&hist[1], &hist[0], sizeof(hist) - sizeof(hist[0]));

    /* memmove() left hist[0] holding the previous current-boot's bytes —
     * including its .crash field. Wipe before overwriting so a fresh
     * FLASH/SW entry doesn't inherit the old row's crash one-liner. */
    memset(&hist[0], 0, sizeof(hist[0]));

    time_t now = 0;
    time(&now);
    hist[0].wallclock = (now > 1700000000) ? (uint32_t)now : 0;
    strncpy(hist[0].reason, rs,          sizeof(hist[0].reason) - 1);
    strncpy(hist[0].who,    who_capture, sizeof(hist[0].who)    - 1);

    nvs_set_blob(h, KEY_BLOB, hist, sizeof(hist));
    nvs_commit(h);
    nvs_close(h);
}

void reset_history_set_current_crash(const char *crash_info)
{
    if (!crash_info || !crash_info[0]) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    reset_history_entry_t hist[RESET_HISTORY_MAX] = {0};
    size_t blob_size = sizeof(hist);
    if (nvs_get_blob(h, KEY_BLOB, hist, &blob_size) == ESP_OK &&
        blob_size == sizeof(hist)) {
        strncpy(hist[0].crash, crash_info, sizeof(hist[0].crash) - 1);
        hist[0].crash[sizeof(hist[0].crash) - 1] = '\0';
        nvs_set_blob(h, KEY_BLOB, hist, sizeof(hist));
        nvs_commit(h);
    }
    nvs_close(h);
}

int reset_history_load(reset_history_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    reset_history_entry_t hist[RESET_HISTORY_MAX] = {0};
    size_t blob_size = sizeof(hist);
    if (nvs_get_blob(h, KEY_BLOB, hist, &blob_size) != ESP_OK) {
        nvs_close(h);
        return 0;
    }
    nvs_close(h);
    sanitize_blob(hist);
    int count = 0;
    int cap = (max < RESET_HISTORY_MAX) ? max : RESET_HISTORY_MAX;
    for (int i = 0; i < cap; i++) {
        if (hist[i].reason[0] == '\0' && hist[i].wallclock == 0) continue;
        out[count++] = hist[i];
    }
    return count;
}

void reset_history_note_restart(const char *who)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_WHO, who ? who : "?");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "*** RESTART: %s ***", who ? who : "?");
    vTaskDelay(pdMS_TO_TICKS(100));   /* let the log line drain over UART */
    esp_restart();
}
