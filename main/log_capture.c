/* In-memory ring-buffer for ESP_LOG output.
 *
 * Forks esp_log_set_vprintf() so every log line lands both in the
 * original UART sink AND in a circular byte buffer the /log web page
 * can render. The hook uses va_copy() so the original vprintf still
 * gets a fresh va_list; without that the second pass walks freed args.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "log_capture.h"

/* Pre-crash RAM survival: a small ring buffer placed in RTC slow RAM
 * via RTC_NOINIT_ATTR. The .rtc_noinit section is preserved across
 * soft resets — including the post-panic reboot the exception handler
 * triggers — so the last few KB of log lines before a PANIC are
 * readable on the next boot. Brown-out / cold power-on wipe it. */
#define PRECRASH_RING_BYTES   4096
#define PRECRASH_MAGIC        0xC0DEAFFEu

typedef struct {
    uint32_t magic;
    uint32_t head;
    uint32_t count;
    char     data[PRECRASH_RING_BYTES];
} precrash_ring_t;

RTC_NOINIT_ATTR static precrash_ring_t s_precrash;
static portMUX_TYPE s_precrash_lock = portMUX_INITIALIZER_UNLOCKED;

#define DEFAULT_BUFFER_BYTES   (16 * 1024)
/* ESP_LOG default formatter emits the whole "color + header + body +
 * reset + \n" sequence in a single vprintf call (LOG_VERSION 1). 256
 * was enough for most lines but truncated the trailing \n when a peer-
 * hostname-heavy line ran over — surfaced as run-together rows in the
 * /log view. 512 leaves headroom for the verbose microlink probes. */
#define LINE_SCRATCH_BYTES     512        /* per-message stack buffer */
#define ROUND_UP_1K(x)         (((x) + 1023) & ~1023u)

static char        *s_buf      = NULL;
static size_t       s_cap      = 0;
static size_t       s_head     = 0;       /* next write position */
static size_t       s_count    = 0;       /* live bytes (<= s_cap) */
static uint64_t     s_seq      = 0;       /* total bytes ever appended */
static portMUX_TYPE s_lock     = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf = NULL;

/* Dual-write append: live RAM ring (s_buf) for the /log poller, and
 * the RTC-slow-RAM ring (s_precrash) for post-panic survival. The two
 * are independent — the live ring grows up to 16 KB, the RTC ring is
 * capped at 4 KB so it fits comfortably in slow RAM. */
static void buffer_append(const char *src, size_t len)
{
    if (len == 0) return;

    /* Live RAM ring */
    if (s_buf && s_cap > 0) {
        const char *src1 = src;
        size_t      len1 = len;
        if (len1 > s_cap) { src1 += (len1 - s_cap); len1 = s_cap; }

        taskENTER_CRITICAL(&s_lock);
        for (size_t i = 0; i < len1; i++) {
            s_buf[s_head] = src1[i];
            s_head = (s_head + 1) % s_cap;
            if (s_count < s_cap) s_count++;
        }
        s_seq += len1;
        taskEXIT_CRITICAL(&s_lock);
    }

    /* RTC slow-RAM ring (panic-survivable) */
    {
        const char *src2 = src;
        size_t      len2 = len;
        if (len2 > PRECRASH_RING_BYTES) {
            src2 += (len2 - PRECRASH_RING_BYTES);
            len2 = PRECRASH_RING_BYTES;
        }
        taskENTER_CRITICAL(&s_precrash_lock);
        for (size_t i = 0; i < len2; i++) {
            s_precrash.data[s_precrash.head] = src2[i];
            s_precrash.head = (s_precrash.head + 1) % PRECRASH_RING_BYTES;
            if (s_precrash.count < PRECRASH_RING_BYTES) s_precrash.count++;
        }
        taskEXIT_CRITICAL(&s_precrash_lock);
    }
}

/* Snapshot of the RTC ring captured at boot, before live logging starts
 * overwriting it. Read by /log as "pre-crash log" when the previous
 * boot crashed. */
static char  *s_snapshot       = NULL;
static size_t s_snapshot_bytes = 0;
static bool   s_snapshot_valid = false;

/* Drop ESP_LOG ANSI color escape sequences in-place. Each color marker
 * has the shape ESC '[' ... 'm', emitted whole inside a single vprintf
 * call by the default ESP-IDF logger. We keep everything else (newlines
 * included), so the result is the same text the user would see on a
 * terminal that doesn't understand colors.
 *
 * Returns the new length. Operates in place — safe because the result
 * is never longer than the input. */
static size_t strip_ansi_inplace(char *buf, size_t len)
{
    size_t w = 0;
    for (size_t r = 0; r < len; ) {
        if (buf[r] == '\033') {
            r++;
            if (r < len && buf[r] == '[') {
                r++;
                while (r < len && buf[r] != 'm') r++;
                if (r < len) r++;  /* skip the 'm' */
            }
            continue;
        }
        buf[w++] = buf[r++];
    }
    return w;
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Make a copy because vsnprintf consumes the va_list; the chained
     * vprintf below still needs the original. */
    va_list copy;
    va_copy(copy, args);
    char scratch[LINE_SCRATCH_BYTES];
    int n = vsnprintf(scratch, sizeof(scratch), fmt, copy);
    va_end(copy);

    if (n > 0) {
        size_t to_copy = (n >= (int)sizeof(scratch)) ? sizeof(scratch) - 1 : (size_t)n;
        to_copy = strip_ansi_inplace(scratch, to_copy);
        /* Guarantee the line is newline-terminated. With LOG_VERSION 1
         * the formatter ends every record with "\033[0m\n", and we
         * keep the '\n'. But if the vsnprintf above truncated past the
         * scratch (or someone routes a raw printf through us without a
         * trailing \n), the absent newline collapses adjacent lines in
         * the /log view. One byte injected here costs nothing. */
        if (to_copy > 0 && scratch[to_copy - 1] != '\n' &&
            to_copy < sizeof(scratch) - 1) {
            scratch[to_copy++] = '\n';
        }
        buffer_append(scratch, to_copy);
    }

    /* Hand off to the original sink so UART output continues unchanged. */
    if (s_orig_vprintf) {
        return s_orig_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

esp_err_t log_capture_init(size_t bytes)
{
    if (s_buf) return ESP_OK;  /* idempotent */
    size_t cap = bytes ? ROUND_UP_1K(bytes) : DEFAULT_BUFFER_BYTES;
    s_buf = malloc(cap);
    if (!s_buf) return ESP_ERR_NO_MEM;
    s_cap   = cap;
    s_head  = 0;
    s_count = 0;

    /* Snapshot the RTC ring BEFORE the live hook starts overwriting it.
     * Validity: a known magic value means the previous boot already
     * populated the ring. After a brown-out / cold power-on the RTC
     * SRAM holds garbage, so the magic mismatch tells us to ignore it
     * and start fresh. */
    if (s_precrash.magic == PRECRASH_MAGIC && s_precrash.count > 0 &&
        s_precrash.count <= PRECRASH_RING_BYTES &&
        s_precrash.head < PRECRASH_RING_BYTES) {
        size_t cnt = s_precrash.count;
        s_snapshot = malloc(cnt + 1);
        if (s_snapshot) {
            size_t start = (s_precrash.head + PRECRASH_RING_BYTES - cnt) % PRECRASH_RING_BYTES;
            size_t written = 0;
            while (written < cnt) {
                size_t chunk = (start + (cnt - written) <= PRECRASH_RING_BYTES)
                               ? (cnt - written)
                               : (PRECRASH_RING_BYTES - start);
                memcpy(s_snapshot + written, s_precrash.data + start, chunk);
                written += chunk;
                start = (start + chunk) % PRECRASH_RING_BYTES;
            }
            s_snapshot[cnt] = '\0';
            s_snapshot_bytes = cnt;
            s_snapshot_valid = true;
        }
    }
    /* Reset the RTC ring for this boot's run. The magic seals it so a
     * future post-panic boot recognises the data as our own. */
    s_precrash.magic = PRECRASH_MAGIC;
    s_precrash.head  = 0;
    s_precrash.count = 0;

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    ESP_LOGI("log_cap", "ring buffer %u bytes installed (precrash snapshot=%u B)",
             (unsigned)cap, (unsigned)s_snapshot_bytes);
    return ESP_OK;
}

size_t log_capture_read_precrash(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    if (!s_snapshot_valid || s_snapshot_bytes == 0 || !s_snapshot) {
        out[0] = '\0';
        return 0;
    }
    size_t cap = out_size - 1;
    size_t want = (s_snapshot_bytes < cap) ? s_snapshot_bytes : cap;
    /* If caller's buffer is too small, prefer the newest tail. */
    size_t skip = s_snapshot_bytes - want;
    memcpy(out, s_snapshot + skip, want);
    out[want] = '\0';
    return want;
}

bool log_capture_have_precrash(void)   { return s_snapshot_valid; }
size_t log_capture_precrash_size(void) { return s_snapshot_bytes; }

void log_capture_clear_precrash(void)
{
    if (s_snapshot) { free(s_snapshot); s_snapshot = NULL; }
    s_snapshot_bytes = 0;
    s_snapshot_valid = false;
}

size_t log_capture_read(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    if (!s_buf || s_count == 0) { out[0] = '\0'; return 0; }

    size_t cap   = out_size - 1;          /* reserve room for NUL */
    size_t want  = (s_count < cap) ? s_count : cap;
    size_t start;
    size_t written = 0;

    taskENTER_CRITICAL(&s_lock);
    /* Oldest byte sits at (head - count) mod cap. When count == cap the
     * ring is full and oldest == head. */
    start = (s_head + s_cap - s_count) % s_cap;
    /* If the caller's buffer is smaller than what we have, prefer the
     * NEWEST `want` bytes (skip the oldest count-want). */
    if (want < s_count) start = (start + (s_count - want)) % s_cap;

    while (written < want) {
        size_t chunk = (start + (want - written) <= s_cap)
                       ? (want - written)
                       : (s_cap - start);
        memcpy(out + written, s_buf + start, chunk);
        written += chunk;
        start = (start + chunk) % s_cap;
    }
    taskEXIT_CRITICAL(&s_lock);
    out[written] = '\0';
    return written;
}

void log_capture_clear(void)
{
    if (!s_buf) return;
    taskENTER_CRITICAL(&s_lock);
    s_head = 0;
    s_count = 0;
    /* Note: s_seq is NOT reset so any /log poller in progress sees a
     * clean re-sync (since_seq < s_seq - s_count) and can rebuild from
     * scratch without confusing wrap detection. */
    taskEXIT_CRITICAL(&s_lock);
}

size_t   log_capture_size(void)     { return s_count; }
size_t   log_capture_capacity(void) { return s_cap; }
uint64_t log_capture_seq(void)      { return s_seq; }

size_t log_capture_read_since(uint64_t since_seq, char *out, size_t out_size,
                              uint64_t *new_seq)
{
    if (!out || out_size == 0) return 0;
    if (new_seq) *new_seq = 0;
    if (!s_buf) { out[0] = '\0'; return 0; }

    size_t cap = out_size - 1;
    size_t written = 0;

    taskENTER_CRITICAL(&s_lock);
    uint64_t cur_seq    = s_seq;
    uint64_t oldest_seq = (s_count < cur_seq) ? (cur_seq - s_count) : 0;

    /* No new data since last poll — fast path. */
    if (since_seq >= cur_seq) {
        taskEXIT_CRITICAL(&s_lock);
        if (new_seq) *new_seq = cur_seq;
        out[0] = '\0';
        return 0;
    }

    /* Effective start: max(since_seq, oldest_seq). When since_seq <
     * oldest_seq the caller has fallen behind — they get the full live
     * buffer and must treat the result as a re-sync. The handler signals
     * this by comparing the requested since_seq against the returned
     * (*new_seq - bytes_written). */
    uint64_t start_seq = (since_seq < oldest_seq) ? oldest_seq : since_seq;
    size_t   want      = (size_t)(cur_seq - start_seq);
    if (want > cap) {
        /* Caller's buffer too small: keep the NEWEST `cap` bytes. */
        start_seq = cur_seq - cap;
        want      = cap;
    }

    /* Translate start_seq into a ring offset. The byte at absolute
     * offset (cur_seq - 1) sits at (s_head + s_cap - 1) % s_cap. So
     * the byte at start_seq is offset (s_head - (cur_seq - start_seq))
     * from the head, modulo cap. */
    size_t back   = (size_t)(cur_seq - start_seq);
    size_t pos    = (s_head + s_cap - back) % s_cap;

    while (written < want) {
        size_t chunk = (pos + (want - written) <= s_cap)
                       ? (want - written)
                       : (s_cap - pos);
        memcpy(out + written, s_buf + pos, chunk);
        written += chunk;
        pos = (pos + chunk) % s_cap;
    }
    taskEXIT_CRITICAL(&s_lock);

    out[written] = '\0';
    if (new_seq) *new_seq = cur_seq;
    return written;
}
