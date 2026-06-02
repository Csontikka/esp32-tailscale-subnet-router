/* SD flight-recorder - persist ESP_LOG output to a microSD card.
 *
 * Architecture:
 *   vprintf hook  ──format──>  FreeRTOS queue  ──fwrite──>  FAT file
 *
 * The vprintf hook never touches the SD card. It accumulates a log line
 * (the WiFi driver splits a line across several vprintf calls), strips
 * ANSI, applies the verbosity filter, and posts it to a queue with a
 * zero timeout (drop-if-full). A dedicated writer task dequeues lines and
 * fwrites them to a rotating set of files under /sdcard/eslog/.
 *
 * The hook chains to the previously-installed vprintf (the UART console),
 * so serial logging keeps working.
 *
 * Filenames are 8.3 only: the build has CONFIG_FATFS_LFN_NONE, so a long
 * name would be silently mangled. Files are "%08u.LOG" (monotonic seq),
 * which sorts lexicographically by age — handy for pruning the oldest.
 *
 * All queue/task/buffers are created on enable and destroyed on disable —
 * zero RAM when the recorder is off. The card stays mounted after init so
 * the list/download/tail endpoints work even while the recorder is idle.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sdlog.h"
#include "microlink.h"   /* Phase B: health-snapshot accessors */

/* Must match NVS_NAMESPACE in main/nvs_params.h. Duplicated to avoid a
 * circular component dependency on main. */
#define SDLOG_NVS_NAMESPACE  "tsr"

/* NVS keys (all under the shared "tsr" namespace). */
#define NVS_KEY_ENABLED  "sdlog_en"     /* u8  */
#define NVS_KEY_VERB     "sdlog_verb"   /* u8  */
#define NVS_KEY_SEQ      "sdlog_seq"    /* u32: next file sequence number */
#define NVS_KEY_BOOT     "sdlog_boot"   /* u32: monotonic boot counter    */
#define NVS_KEY_MAXMB    "sdlog_maxmb"  /* u16: ~max files (≈1 MB each)   */
#define NVS_KEY_CONLVL   "sdlog_con"    /* u8: console output level (SDLOG_LVL_*) */
#define NVS_KEY_SDLVL    "sdlog_sdl"    /* u8: SD recorder level    (SDLOG_LVL_*) */

/* Card pins — onboard microSD wired as SDMMC 1-bit on custom GPIOs that
 * dodge the octal-PSRAM pads (35/36/37). 39/40 double as JTAG MTCK/MTDO
 * but JTAG is unused on this board. On-board pull-ups present. */
#define SDLOG_PIN_CLK   39
#define SDLOG_PIN_CMD   38
#define SDLOG_PIN_D0    40

/* Conservative clock for a GPIO-matrix-routed slot. The recorder writes a
 * trickle of log lines, so speed is irrelevant; reliability on first
 * bring-up wins. Raise toward SDMMC_FREQ_DEFAULT (20 MHz) later if the
 * download endpoint feels slow and the card proves stable. */
#define SDLOG_SD_FREQ_KHZ   SDMMC_FREQ_DEFAULT

#define SDLOG_MOUNT_POINT   "/sdcard"
#define SDLOG_DIR           "/sdcard/eslog"

/* Per-line ceiling and queue depth (256-byte log lines). */
#define SDLOG_LINE_MAX      256
#define SDLOG_QUEUE_DEPTH   32

/* Rotate the active file at ~1 MB; keep at most this many files. */
#define SDLOG_FILE_MAX_BYTES   (1024 * 1024)
#define SDLOG_DEFAULT_MAXFILES 128

/* Flush cadence: fsync the active file at most this often (ms). */
#define SDLOG_FSYNC_PERIOD_MS  2000
/* Writer wakes at least this often even with no traffic — also bounds the
 * SNAP cadence, so it must be <= the anomaly SNAP period (1 s). */
#define SDLOG_TICK_MS          1000

/* Phase B health snapshot: a SNAP line every 10 s normally, every 1 s when
 * the DERP heartbeat age crosses the anomaly threshold (the wedge moment).
 * Written straight to the file (not via ESP_LOG) so it lands even when the
 * verbosity filter would drop everything else. */
#define SDLOG_SNAP_PERIOD_MS         10000
#define SDLOG_SNAP_ANOMALY_PERIOD_MS 1000
#define SDLOG_DERP_HB_ANOMALY_S      15
/* coord_age anomaly: a healthy long-poll resets ctrl_last_rx every ~5 s
 * (our PING → server PONG). 30 s of server silence = 6 missed round-trips,
 * the silent control-plane wedge — tighten SNAP cadence to catch the climb. */
#define SDLOG_CTRL_RX_ANOMALY_S      30

static const char *TAG = "sdlog";

/* Sentinel msg.len values posted through the queue (real log lines are
 * 1..SDLOG_LINE_MAX): 0 = poison pill (writer drains + exits), 0xFFFF =
 * flush marker (writer fsyncs the active file then signals s_flush_done). */
#define SDLOG_MSG_POISON  0u
#define SDLOG_MSG_FLUSH   0xFFFFu

/* One queued log line. */
typedef struct {
    uint16_t len;                 /* bytes used in data (includes the \n) */
    char     data[SDLOG_LINE_MAX];
} sdlog_msg_t;

/* ---- State ---- */

static atomic_bool s_present = false;   /* card mounted              */
static atomic_bool s_enabled = false;   /* recorder active           */
static _Atomic uint8_t s_sd_level      = SDLOG_LVL_WARN;  /* SD recorder sink level (read live) */
static _Atomic uint8_t s_console_level = SDLOG_LVL_WARN;  /* console sink level (read live by the hook) */
static uint16_t    s_max_files = SDLOG_DEFAULT_MAXFILES;
static sdmmc_card_t *s_card = NULL;

/* Config / lifecycle guard (enable/disable serialization). */
static SemaphoreHandle_t s_state_mutex = NULL;
/* Serializes all FAT access between the writer task and the read APIs. */
static SemaphoreHandle_t s_file_mutex = NULL;

/* Writer task + queue. */
static QueueHandle_t s_queue = NULL;
static TaskHandle_t  s_writer_task = NULL;
/* Signalled by the writer once a flush marker has been fsynced, so
 * sdlog_flush() can block until the queue tail is durably on the card. */
static SemaphoreHandle_t s_flush_done = NULL;

/* Active file — writer-owned, but read APIs touch the FS under
 * s_file_mutex too, so guard FILE* access with it. */
static FILE    *s_fp = NULL;
static uint32_t s_cur_bytes = 0;        /* bytes in the active file       */
static uint32_t s_next_seq = 1;         /* next filename sequence number  */
static uint32_t s_boot_count = 0;       /* this boot's marker number      */
static char     s_cur_name[SDLOG_NAME_MAX] = {0};
static uint64_t s_bytes_written = 0;    /* total this boot                */
static _Atomic uint32_t s_dropped = 0;  /* lines dropped (queue full)     */

/* Formatting state for the hook (non-blocking trylock). */
static SemaphoreHandle_t s_fmt_mutex = NULL;
static char *s_rawbuf = NULL;           /* line accumulation              */
static char *s_pktbuf = NULL;           /* cleaned line                   */
static int   s_rawpos = 0;

static vprintf_like_t s_original_vprintf = NULL;

/* microlink handle for the Phase B SNAP line. Set by main once microlink
 * is initialised; NULL until then (SNAP degrades to heap/uptime only). */
static microlink_t *s_ml = NULL;

/* Per-task re-entrancy guard. Set in the writer around FAT ops so the
 * recorder's own log lines (and any FATFS/SDMMC warnings emitted during a
 * write) don't feed back into the queue. */
static _Thread_local bool tl_in_sdlog = false;

/* ---- NVS ---- */

static void load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SDLOG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return;
    uint8_t en = 0, cl = SDLOG_LVL_WARN, sl = SDLOG_LVL_WARN;
    nvs_get_u8(nvs, NVS_KEY_ENABLED, &en);
    nvs_get_u8(nvs, NVS_KEY_CONLVL, &cl);   /* absent → stays WARN (default) */
    nvs_get_u8(nvs, NVS_KEY_SDLVL,  &sl);   /* absent → stays WARN (default) */
    nvs_get_u32(nvs, NVS_KEY_SEQ, &s_next_seq);
    uint16_t mm = 0;
    if (nvs_get_u16(nvs, NVS_KEY_MAXMB, &mm) == ESP_OK && mm > 0)
        s_max_files = mm;
    nvs_close(nvs);
    atomic_store(&s_enabled, en != 0);
    atomic_store(&s_console_level, (cl <= SDLOG_LVL_INFO) ? cl : SDLOG_LVL_WARN);
    atomic_store(&s_sd_level,
                 (sl >= SDLOG_LVL_ERROR && sl <= SDLOG_LVL_INFO) ? sl : SDLOG_LVL_WARN);
    if (s_next_seq == 0) s_next_seq = 1;
}

/* Set the runtime master log level = max(active sink levels), via
 * esp_log_level_set("*", ...). A line is only formatted + handed to the
 * vprintf hook when at least one sink wants it, so INFO stays
 * compiled-in-but-suppressed (~0 cost) while both sinks are at WARN, yet a
 * sink can pull INFO on demand. SDLOG_LVL_* is esp_log_level_t-aligned
 * (NONE/ERROR/WARN/INFO = 0/1/2/3). Called at init and on any sink change. */
static void apply_master_level(void)
{
    uint8_t con = atomic_load(&s_console_level);
    uint8_t sd  = atomic_load(&s_enabled) ? atomic_load(&s_sd_level) : SDLOG_LVL_OFF;
    uint8_t m   = (con > sd) ? con : sd;
    esp_log_level_set("*", (esp_log_level_t)m);
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t nvs;
    if (nvs_open(SDLOG_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, key, v);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void save_seq(uint32_t v)
{
    nvs_handle_t nvs;
    if (nvs_open(SDLOG_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u32(nvs, NVS_KEY_SEQ, v);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static uint32_t bump_boot_counter(void)
{
    nvs_handle_t nvs;
    uint32_t boot = 0;
    if (nvs_open(SDLOG_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return 0;
    nvs_get_u32(nvs, NVS_KEY_BOOT, &boot);
    boot++;
    nvs_set_u32(nvs, NVS_KEY_BOOT, boot);
    nvs_commit(nvs);
    nvs_close(nvs);
    return boot;
}

/* ---- ANSI / level helpers ---- */

static char extract_level(const char *msg, const char **clean_start)
{
    const char *p = msg;
    char level = '\0';   /* '\0' = no recognised level → raw/un-leveled output */
    if (p[0] == '\033' && p[1] == '[') {
        p += 2;
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    if (*p && (p[1] == ' ' || p[1] == '('))
        level = *p;
    *clean_start = p;
    return level;
}

/* True if a line whose level letter is `lv` ('E'/'W'/'I'/'D'/'V', or '\0' for
 * raw/un-leveled output) should be emitted at a sink threshold `thr`
 * (SDLOG_LVL_*). Raw output is kept at every threshold except OFF, so panic /
 * boot / bare-printf lines are never hidden unless the sink is fully off. */
static inline bool level_passes(char lv, uint8_t thr)
{
    if (thr == SDLOG_LVL_OFF) return false;
    switch (lv) {
        case 'E': return thr >= SDLOG_LVL_ERROR;
        case 'W': return thr >= SDLOG_LVL_WARN;
        case 'I': return thr >= SDLOG_LVL_INFO;
        case 'D': case 'V': return false;   /* compiled out (ceiling = INFO) */
        default:  return true;              /* raw / un-leveled: keep unless OFF */
    }
}

static int strip_ansi(const char *src, char *dst, int max_len)
{
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 1; ) {
        if (src[i] == '\033') {
            i++;
            if (src[i] == '[') {
                i++;
                while (src[i] && src[i] != 'm') i++;
                if (src[i] == 'm') i++;
            }
        } else {
            dst[j++] = src[i++];
        }
    }
    while (j > 0 && (dst[j-1] == '\n' || dst[j-1] == '\r'))
        j--;
    dst[j] = '\0';
    return j;
}

/* ---- File management (all callers hold s_file_mutex) ---- */

static void build_path(const char *name, char *out, size_t out_sz)
{
    /* Cap the name to one 8.3 slot (callers only ever pass names from our
     * own dir); the precision also lets the format-truncation analyzer
     * prove the result fits the fixed-size path buffers. */
    snprintf(out, out_sz, "%s/%.*s", SDLOG_DIR, (int)(SDLOG_NAME_MAX - 1), name);
}

/* Delete the oldest files until at most s_max_files remain. Names sort
 * lexicographically by age (zero-padded seq), so the smallest name is the
 * oldest. One scan per deletion — n is tiny (≤ a few hundred). */
static void prune_old_files(void)
{
    for (;;) {
        DIR *d = opendir(SDLOG_DIR);
        if (!d) return;
        int count = 0;
        char oldest[SDLOG_NAME_MAX] = {0};
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".LOG") != 0) continue;
            count++;
            if (oldest[0] == '\0' || strcasecmp(e->d_name, oldest) < 0) {
                strncpy(oldest, e->d_name, sizeof(oldest) - 1);
                oldest[sizeof(oldest) - 1] = '\0';
            }
        }
        closedir(d);
        if (count <= (int)s_max_files || oldest[0] == '\0')
            return;
        char path[64];
        build_path(oldest, path, sizeof path);
        unlink(path);
    }
}

static void fsync_active(void)
{
    if (!s_fp) return;
    fflush(s_fp);
    int fd = fileno(s_fp);
    if (fd >= 0) fsync(fd);
}

/* Open a fresh file and write its header line. boot=true → boot marker,
 * false → a rotation marker. Returns ESP_OK on success. */
static esp_err_t open_new_file(bool boot, uint32_t boot_count)
{
    if (s_fp) {
        fsync_active();
        fclose(s_fp);
        s_fp = NULL;
    }

    char name[SDLOG_NAME_MAX];
    snprintf(name, sizeof name, "%08u.LOG", (unsigned)s_next_seq);
    char path[64];
    build_path(name, path, sizeof path);

    s_fp = fopen(path, "w");
    if (!s_fp) {
        ESP_LOGE(TAG, "fopen %s failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    strncpy(s_cur_name, name, sizeof(s_cur_name) - 1);
    s_cur_name[sizeof(s_cur_name) - 1] = '\0';
    s_cur_bytes = 0;

    /* Advance + persist the sequence so the next file (and the next boot)
     * gets a unique, monotonically increasing name. */
    s_next_seq++;
    save_seq(s_next_seq);

    char hdr[160];
    int n;
    if (boot) {
        const esp_app_desc_t *desc = esp_app_get_description();
        n = snprintf(hdr, sizeof hdr,
            "==== BOOT #%u reason=%d idf=%s fw=%s heap=%u/%u ====\n",
            (unsigned)boot_count, (int)esp_reset_reason(),
            esp_get_idf_version(),
            desc ? desc->version : "?",
            (unsigned)esp_get_free_heap_size(),
            (unsigned)esp_get_minimum_free_heap_size());
    } else {
        n = snprintf(hdr, sizeof hdr,
            "==== ROTATE seq=%u uptime=%llus ====\n",
            (unsigned)(s_next_seq - 1),
            (unsigned long long)(esp_timer_get_time() / 1000000));
    }
    if (n > 0) {
        fwrite(hdr, 1, (size_t)n, s_fp);
        s_cur_bytes += (uint32_t)n;
        s_bytes_written += (uint32_t)n;
    }
    fsync_active();
    prune_old_files();
    return ESP_OK;
}

/* ---- Phase B: health snapshot ---- */

/* FreeRTOS eTaskState (0..4), or -1 for a missing handle, to one char. */
static char task_state_char(int st)
{
    switch (st) {
        case 0:  return 'R';   /* eRunning   */
        case 1:  return 'r';   /* eReady     */
        case 2:  return 'B';   /* eBlocked   */
        case 3:  return 'S';   /* eSuspended */
        case 4:  return 'D';   /* eDeleted   */
        case -1: return '-';   /* no handle  */
        default: return '?';
    }
}

static const char *ml_state_str(int s)
{
    switch (s) {
        case 0:  return "IDLE";
        case 1:  return "WIFI";
        case 2:  return "CONN";
        case 3:  return "REG";
        case 4:  return "UP";
        case 5:  return "RECON";
        case 6:  return "ERR";
        default: return "?";
    }
}

/* Format + fwrite one SNAP line. Caller holds s_file_mutex, s_fp is valid,
 * and tl_in_sdlog is set. now_ms/hb_age_s are precomputed by the caller. */
static void emit_snap(uint64_t now_ms, uint32_t hb_age_s, uint32_t coord_age_s)
{
    uint32_t up = (uint32_t)(now_ms / 1000);
    uint32_t heap_int = esp_get_free_heap_size();
    uint32_t heap_min = esp_get_minimum_free_heap_size();
    uint32_t heap_spi = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* Internal DRAM specifically — WiFi TX descriptors + lwIP pbufs live here,
     * NOT in SPIRAM. This is what actually exhausts under a forwarding load
     * burst (speedtest + rapid churn): sends then fail with ENOMEM while
     * SPIRAM still looks plentiful. dram_min is the low-water mark, the real
     * ENOMEM proximity signal the SPIRAM figures above completely hide. */
    uint32_t dram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t dram_min  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    char coord = '-', derp = '-', wg = '-', netio = '-';
    int ml_st = -1, peers = 0, online = 0;
    if (s_ml) {
        microlink_task_states_t ts;
        microlink_get_task_states(s_ml, &ts);
        coord = task_state_char(ts.coord);
        derp  = task_state_char(ts.derp_tx);
        wg    = task_state_char(ts.wg_mgr);
        netio = task_state_char(ts.net_io);
        microlink_diag_t d;
        if (microlink_get_diag(s_ml, &d) == ESP_OK) {
            ml_st  = (int)d.state;
            peers  = d.peer_count;
            online = d.peer_online;
        }
    }

    char line[256];
    int n = snprintf(line, sizeof line,
        "SNAP up=%us heap=%u/%uk min=%uk idram=%uk/min%uk | coord=%c derp=%c wg=%c netio=%c | "
        "derp_hb_age=%us coord_age=%us ml=%s peers=%d/%d | dropped=%u\n",
        (unsigned)up, (unsigned)heap_int, (unsigned)(heap_spi / 1024),
        (unsigned)(heap_min / 1024),
        (unsigned)(dram_free / 1024), (unsigned)(dram_min / 1024),
        coord, derp, wg, netio,
        (unsigned)hb_age_s, (unsigned)coord_age_s, ml_state_str(ml_st), online, peers,
        (unsigned)atomic_load(&s_dropped));
    if (n > 0) {
        if (n > (int)sizeof line) n = sizeof line;
        size_t w = fwrite(line, 1, (size_t)n, s_fp);
        s_cur_bytes     += (uint32_t)w;
        s_bytes_written += (uint32_t)w;
    }
}

/* ---- Writer task ---- */

static void writer_task(void *arg)
{
    (void)arg;
    int64_t last_fsync_us = esp_timer_get_time();
    int64_t last_snap_us  = esp_timer_get_time();
    bool dirty = false;

    /* Open the per-boot file first so the marker lands even if nothing
     * else is ever logged. */
    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    tl_in_sdlog = true;
    open_new_file(true, s_boot_count);
    tl_in_sdlog = false;
    xSemaphoreGive(s_file_mutex);

    sdlog_msg_t msg;
    for (;;) {
        BaseType_t got = xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(SDLOG_TICK_MS));

        if (got == pdTRUE && msg.len == SDLOG_MSG_POISON)
            break;                         /* poison pill */

        if (got == pdTRUE && msg.len == SDLOG_MSG_FLUSH) {
            /* Flush marker: everything enqueued before it has already been
             * written (the queue is FIFO), so fsync the active file durably
             * and wake the sdlog_flush() caller. */
            xSemaphoreTake(s_file_mutex, portMAX_DELAY);
            tl_in_sdlog = true;
            if (s_fp) fsync_active();
            tl_in_sdlog = false;
            xSemaphoreGive(s_file_mutex);
            if (s_flush_done) xSemaphoreGive(s_flush_done);
            continue;
        }

        xSemaphoreTake(s_file_mutex, portMAX_DELAY);
        tl_in_sdlog = true;

        /* Reopen if the active file went away (e.g. after an erase). */
        if (!s_fp) {
            open_new_file(false, s_boot_count);
            last_fsync_us = esp_timer_get_time();
            dirty = false;
        }

        if (got == pdTRUE && s_fp && msg.len > 0) {
            size_t w = fwrite(msg.data, 1, msg.len, s_fp);
            s_cur_bytes     += (uint32_t)w;
            s_bytes_written += (uint32_t)w;
            dirty = true;
            if (s_cur_bytes >= SDLOG_FILE_MAX_BYTES) {
                open_new_file(false, s_boot_count);  /* rotation marker */
                dirty = false;
                last_fsync_us = esp_timer_get_time();
            }
        }

        int64_t now = esp_timer_get_time();

        /* Phase B SNAP: a periodic health line straight to the file. The
         * cadence tightens to 1 s once the DERP heartbeat age crosses the
         * anomaly threshold, capturing the wedge moment + the climbing age
         * even while coord/derp are blocked and emitting nothing. */
        uint64_t now_ms = (uint64_t)(now / 1000);
        uint64_t hb = s_ml ? microlink_get_last_derp_heartbeat_ms(s_ml) : 0;
        uint32_t hb_age = (hb && now_ms > hb) ? (uint32_t)((now_ms - hb) / 1000) : 0;
        /* coord_age: seconds since the control plane last sent us a frame.
         * Climbs during the silent wedge even though derp_hb_age stays 0 and
         * microlink still reports CONNECTED — the divergence is the signature. */
        uint64_t cr = s_ml ? microlink_get_ctrl_last_rx_ms(s_ml) : 0;
        uint32_t coord_age = (cr && now_ms > cr) ? (uint32_t)((now_ms - cr) / 1000) : 0;
        bool anomaly = (hb_age > SDLOG_DERP_HB_ANOMALY_S) ||
                       (coord_age > SDLOG_CTRL_RX_ANOMALY_S);
        int64_t snap_iv_us = anomaly
                           ? (int64_t)SDLOG_SNAP_ANOMALY_PERIOD_MS * 1000
                           : (int64_t)SDLOG_SNAP_PERIOD_MS * 1000;
        if (s_fp && (now - last_snap_us) >= snap_iv_us) {
            emit_snap(now_ms, hb_age, coord_age);
            last_snap_us = now;
            dirty = true;
        }

        if (dirty && (now - last_fsync_us) >= SDLOG_FSYNC_PERIOD_MS * 1000) {
            fsync_active();
            dirty = false;
            last_fsync_us = now;
        }

        tl_in_sdlog = false;
        xSemaphoreGive(s_file_mutex);
    }

    /* Drain + close on shutdown. */
    xSemaphoreTake(s_file_mutex, portMAX_DELAY);
    tl_in_sdlog = true;
    if (s_fp) {
        fsync_active();
        fclose(s_fp);
        s_fp = NULL;
    }
    s_cur_name[0] = '\0';
    tl_in_sdlog = false;
    xSemaphoreGive(s_file_mutex);

    s_writer_task = NULL;
    vTaskDelete(NULL);
}

/* ---- vprintf hook (never touches the card) ---- */

static int sdlog_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    /* Console (UART) gate — INDEPENDENT of the SD recorder below. The
     * level letter ('W'/'E') is a literal at the start of fmt (ESP-IDF embeds
     * it in LOG_FORMAT). Raw printf()/panic/boot lines parse as non-W/E and
     * are ALWAYS kept unless the console is fully OFF, so critical un-leveled
     * output is never hidden. Fragment-aware: the WiFi driver splits a line
     * across vprintf calls, so we only (re)decide at a line start and the
     * continuation fragments inherit it. esp_log serialises hook calls, so
     * the static line state is safe. */
    static bool s_con_emit = true;
    static bool s_con_at_line_start = true;
    uint8_t clvl = atomic_load(&s_console_level);
    if (s_con_at_line_start) {
        const char *cs;
        s_con_emit = level_passes(extract_level(fmt, &cs), clvl);
    }
    {
        size_t fl = strlen(fmt);
        s_con_at_line_start = (fl > 0 && fmt[fl - 1] == '\n');
    }

    int ret = 0;
    if (s_con_emit) {
        ret = s_original_vprintf ? s_original_vprintf(fmt, args)
                                 : vprintf(fmt, args);
    }

    if (!atomic_load(&s_enabled) || tl_in_sdlog || !s_queue) {
        va_end(args_copy);
        return ret;
    }
    tl_in_sdlog = true;

    if (xSemaphoreTake(s_fmt_mutex, 0) == pdTRUE) {
        if (s_rawbuf && s_pktbuf) {
            int space = SDLOG_LINE_MAX - s_rawpos - 1;
            if (space > 0) {
                int n = vsnprintf(s_rawbuf + s_rawpos, space + 1, fmt, args_copy);
                if (n > 0) s_rawpos += (n > space) ? space : n;
            }

            /* Flush on a completed line OR when the buffer is full. The
             * full-buffer case is essential: a single log line >= 256 B
             * (or any never-terminated fragment stream) would otherwise
             * pin s_rawpos at the max with no '\n' to trigger a flush,
             * silently wedging the recorder forever. Treat the 255-byte
             * chunk as a line so capture always makes progress. */
            bool buf_full = (s_rawpos >= SDLOG_LINE_MAX - 1);
            if (s_rawpos > 0 && (buf_full || memchr(s_rawbuf, '\n', s_rawpos))) {
                s_rawbuf[s_rawpos] = '\0';
                const char *clean_start;
                char level = extract_level(s_rawbuf, &clean_start);
                int clean_len = strip_ansi(clean_start, s_pktbuf, SDLOG_LINE_MAX);
                s_rawpos = 0;

                bool pass = level_passes(level, atomic_load(&s_sd_level));

                if (clean_len > 0 && pass) {
                    sdlog_msg_t m;
                    int dl = clean_len;
                    if (dl > SDLOG_LINE_MAX - 1) dl = SDLOG_LINE_MAX - 1;
                    memcpy(m.data, s_pktbuf, dl);
                    m.data[dl] = '\n';
                    m.len = (uint16_t)(dl + 1);
                    if (xQueueSend(s_queue, &m, 0) != pdTRUE)
                        atomic_fetch_add(&s_dropped, 1);
                }
            }
        }
        xSemaphoreGive(s_fmt_mutex);
    }

    va_end(args_copy);
    tl_in_sdlog = false;
    return ret;
}

/* ---- Resource lifecycle ---- */

static esp_err_t mount_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDLOG_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SDLOG_PIN_CLK;
    slot.cmd = SDLOG_PIN_CMD;
    slot.d0  = SDLOG_PIN_D0;
    slot.width = 1;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDLOG_MOUNT_POINT, &host,
                                            &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no microSD (%s) — recorder dark", esp_err_to_name(err));
        return err;
    }

    mkdir(SDLOG_DIR, 0777);   /* mode ignored by FAT; ENOENT-safe if exists */
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SDLOG_MOUNT_POINT, &total, &freeb);
    /* WARN level so the mount result is visible on the default (WARN)
     * console — important boot state for a flight-recorder bring-up. */
    ESP_LOGW(TAG, "microSD mounted: %llu MB total, %llu MB free",
             total >> 20, freeb >> 20);
    atomic_store(&s_present, true);
    return ESP_OK;
}

esp_err_t sdlog_flush(void)
{
    /* Drain the queue onto the card and fsync, synchronously. Call before a
     * deliberate esp_restart() so the flight recorder keeps the last seconds
     * of log before a controlled reboot — otherwise the queue tail plus the
     * not-yet-fsynced FATFS buffer (up to the 2 s fsync period) are lost.
     * Posts a flush marker so FIFO ordering guarantees every line queued
     * before this call is written first, then waits for the writer's fsync. */
    /* Hard upper bounds so a stalled card (fsync hung in the driver, full
     * queue with a wedged writer) can never block the caller's reboot. The
     * caller treats this as best-effort and reboots regardless of the return.
     * Worst case added latency ≈ SEND + WAIT below. */
    const TickType_t kSendTo = pdMS_TO_TICKS(250);    /* queue the marker   */
    const TickType_t kWaitTo = pdMS_TO_TICKS(1500);   /* writer drains+fsync */

    if (!s_queue || !s_writer_task || !s_flush_done) return ESP_ERR_INVALID_STATE;
    (void)xSemaphoreTake(s_flush_done, 0);   /* clear any stale completion */
    sdlog_msg_t marker = { .len = SDLOG_MSG_FLUSH };
    if (xQueueSend(s_queue, &marker, kSendTo) != pdTRUE)
        return ESP_FAIL;
    /* The writer may have a backlog ahead of our marker — bounded wait. */
    return (xSemaphoreTake(s_flush_done, kWaitTo) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void start_writer(void)
{
    if (!s_queue)    s_queue = xQueueCreate(SDLOG_QUEUE_DEPTH, sizeof(sdlog_msg_t));
    if (!s_flush_done) s_flush_done = xSemaphoreCreateBinary();
    if (!s_fmt_mutex) s_fmt_mutex = xSemaphoreCreateMutex();
    if (!s_rawbuf) { s_rawbuf = malloc(SDLOG_LINE_MAX); s_rawpos = 0; }
    if (!s_pktbuf)   s_pktbuf = malloc(SDLOG_LINE_MAX);

    if (!s_writer_task && s_queue) {
        /* 5120-byte stack: this task does buffered
         * stdio (FATFS sector cache + 256 B line scratch) plus ESP_LOG. */
        xTaskCreate(writer_task, "sdlog_wr", 5120, NULL, 4, &s_writer_task);
    }
}

static void stop_writer(void)
{
    if (s_queue && s_writer_task) {
        sdlog_msg_t poison = { .len = 0 };
        xQueueSend(s_queue, &poison, pdMS_TO_TICKS(100));
        /* Wait for the task to drain + close the file. */
        for (int i = 0; i < 40 && s_writer_task; i++)
            vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    free(s_rawbuf); s_rawbuf = NULL; s_rawpos = 0;
    free(s_pktbuf); s_pktbuf = NULL;
}

/* ---- Public API ---- */

void sdlog_set_microlink(void *ml)
{
    s_ml = (microlink_t *)ml;
}

esp_err_t sdlog_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_file_mutex  = xSemaphoreCreateMutex();
    if (!s_state_mutex || !s_file_mutex) return ESP_ERR_NO_MEM;

    load_config();
    mount_card();   /* sets s_present; failure is non-fatal */

    /* Install the hook unconditionally so runtime enable/disable works.
     * Runs after the UART console is up, so the saved fn is its hook. */
    s_original_vprintf = esp_log_set_vprintf(sdlog_vprintf);

    if (atomic_load(&s_present) && atomic_load(&s_enabled)) {
        s_boot_count = bump_boot_counter();
        ESP_LOGW(TAG, "recorder enabled at boot — sd_level %u", atomic_load(&s_sd_level));
        start_writer();
    } else {
        /* Persisted-enabled but no card: keep the flag off at runtime so
         * the hook fast-exits and the UI shows "no card". */
        if (!atomic_load(&s_present)) atomic_store(&s_enabled, false);
    }
    apply_master_level();   /* runtime master = max(console, active SD) */
    return ESP_OK;
}

bool sdlog_card_present(void) { return atomic_load(&s_present); }
bool sdlog_is_enabled(void)   { return atomic_load(&s_enabled); }

esp_err_t sdlog_enable(uint8_t sd_level)
{
    if (!atomic_load(&s_present)) return ESP_ERR_INVALID_STATE;
    if (sd_level < SDLOG_LVL_ERROR) sd_level = SDLOG_LVL_ERROR;  /* OFF == disable() */
    if (sd_level > SDLOG_LVL_INFO)  sd_level = SDLOG_LVL_INFO;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    atomic_store(&s_sd_level, sd_level);
    bool was = atomic_exchange(&s_enabled, true);
    if (!was) { s_boot_count = bump_boot_counter(); start_writer(); }
    xSemaphoreGive(s_state_mutex);

    save_u8(NVS_KEY_SDLVL, sd_level);
    save_u8(NVS_KEY_ENABLED, 1);
    apply_master_level();
    ESP_LOGW(TAG, "recorder enabled (sd_level %u)", sd_level);
    return ESP_OK;
}

esp_err_t sdlog_disable(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    atomic_store(&s_enabled, false);
    stop_writer();
    xSemaphoreGive(s_state_mutex);

    save_u8(NVS_KEY_ENABLED, 0);
    apply_master_level();   /* SD off → master drops to the console level */
    ESP_LOGW(TAG, "recorder disabled");
    return ESP_OK;
}

esp_err_t sdlog_set_console_level(uint8_t level)
{
    if (level > SDLOG_LVL_INFO) level = SDLOG_LVL_INFO;
    ESP_LOGW(TAG, "console level -> %u (0=off 1=err 2=warn 3=info)", level);  /* logged at the OLD level so it's visible */
    atomic_store(&s_console_level, level);   /* live: the hook reads it per line */
    save_u8(NVS_KEY_CONLVL, level);
    apply_master_level();
    return ESP_OK;
}

void sdlog_get_status(sdlog_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->present       = atomic_load(&s_present);
    out->enabled       = atomic_load(&s_enabled);
    out->sd_level      = atomic_load(&s_sd_level);
    out->console_level = atomic_load(&s_console_level);
    out->dropped       = atomic_load(&s_dropped);

    if (out->present) {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(SDLOG_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            out->card_mb = (uint32_t)(total >> 20);
            out->free_mb = (uint32_t)(freeb >> 20);
        }
    }

    /* File listing stats + active file under the FAT lock. */
    if (out->present && s_file_mutex &&
        xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        strncpy(out->cur_file, s_cur_name, sizeof(out->cur_file) - 1);
        out->bytes_written = s_bytes_written;
        DIR *d = opendir(SDLOG_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_type == DT_DIR) continue;
                const char *dot = strrchr(e->d_name, '.');
                if (dot && strcasecmp(dot, ".LOG") == 0) out->file_count++;
            }
            closedir(d);
        }
        xSemaphoreGive(s_file_mutex);
    }
}

/* Reject anything that isn't a bare 8.3-ish basename in our log dir. */
static bool name_is_safe(const char *name)
{
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len >= SDLOG_NAME_MAX) return false;
    if (strstr(name, "..")) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static int cmp_name_desc(const void *a, const void *b)
{
    const sdlog_file_info_t *x = a, *y = b;
    return strcasecmp(y->name, x->name);   /* newest (largest seq) first */
}

int sdlog_list_files(sdlog_file_info_t *out, int max_entries)
{
    if (!out || max_entries <= 0) return -1;
    if (!atomic_load(&s_present) || !s_file_mutex) return -1;
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return -1;

    int n = 0;
    DIR *d = opendir(SDLOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max_entries) {
            if (e->d_type == DT_DIR) continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".LOG") != 0) continue;
            strncpy(out[n].name, e->d_name, SDLOG_NAME_MAX - 1);
            out[n].name[SDLOG_NAME_MAX - 1] = '\0';
            char path[64];
            build_path(e->d_name, path, sizeof path);
            struct stat st;
            out[n].size  = (stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;
            out[n].mtime = (stat(path, &st) == 0) ? (uint32_t)st.st_mtime : 0;
            n++;
        }
        closedir(d);
    }
    xSemaphoreGive(s_file_mutex);

    if (n > 1) qsort(out, n, sizeof(out[0]), cmp_name_desc);
    return n;
}

esp_err_t sdlog_read_file(const char *name, sdlog_chunk_cb cb, void *ctx)
{
    if (!name_is_safe(name) || !cb) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_present) || !s_file_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    char path[64];
    build_path(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { xSemaphoreGive(s_file_mutex); return ESP_ERR_NOT_FOUND; }

    esp_err_t err = ESP_OK;
    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        err = cb(ctx, buf, r);
        if (err != ESP_OK) break;
    }
    fclose(f);
    xSemaphoreGive(s_file_mutex);
    return err;
}

esp_err_t sdlog_tail_file(const char *name, int n_lines,
                          char *buf, size_t buf_size, size_t *out_len)
{
    if (!name_is_safe(name) || !buf || buf_size < 2) return ESP_ERR_INVALID_ARG;
    if (n_lines <= 0) n_lines = 50;
    if (!atomic_load(&s_present) || !s_file_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    char path[64];
    build_path(name, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { xSemaphoreGive(s_file_mutex); return ESP_ERR_NOT_FOUND; }

    /* Read at most the last (buf_size-1) bytes, then trim to n_lines. */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    long want = (long)buf_size - 1;
    long start = (fsz > want) ? fsz - want : 0;
    fseek(f, start, SEEK_SET);
    size_t got = fread(buf, 1, buf_size - 1, f);
    fclose(f);
    xSemaphoreGive(s_file_mutex);
    buf[got] = '\0';

    /* Walk back from the end counting newlines to keep only n_lines. */
    int seen = 0;
    size_t cut = 0;
    for (long i = (long)got - 1; i >= 0; i--) {
        if (buf[i] == '\n') {
            seen++;
            if (seen > n_lines) { cut = (size_t)(i + 1); break; }
        }
    }
    if (cut > 0) {
        memmove(buf, buf + cut, got - cut);
        got -= cut;
        buf[got] = '\0';
    }
    if (out_len) *out_len = got;
    return ESP_OK;
}

esp_err_t sdlog_erase_all(void)
{
    if (!atomic_load(&s_present) || !s_file_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(3000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    /* Close the active file first so we don't unlink an open handle. */
    tl_in_sdlog = true;
    if (s_fp) { fsync_active(); fclose(s_fp); s_fp = NULL; s_cur_name[0] = '\0'; }

    DIR *d = opendir(SDLOG_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcasecmp(dot, ".LOG") != 0) continue;
            char path[64];
            build_path(e->d_name, path, sizeof path);
            unlink(path);
        }
        closedir(d);
    }
    tl_in_sdlog = false;
    xSemaphoreGive(s_file_mutex);

    /* If the recorder is still enabled, the writer task notices s_fp==NULL
     * on its next tick (≤ SDLOG_TICK_MS) and opens a fresh file. */
    return ESP_OK;
}
