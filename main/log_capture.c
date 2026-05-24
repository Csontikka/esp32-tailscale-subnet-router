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
#include "esp_heap_caps.h"
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

/* Lock-free direct write into the RTC ring. Called from
 * __wrap_panic_print_char while the IDF panic handler is dumping the
 * crash banner — every other task is frozen at that point so the lack
 * of locking is fine. Kept short and free of any other code path so
 * the call is safe even before the cache is fully online. */
void log_capture_append_panic(const char *s, size_t len)
{
    if (!s || len == 0) return;
    if (len > PRECRASH_RING_BYTES) {
        s   += (len - PRECRASH_RING_BYTES);
        len  = PRECRASH_RING_BYTES;
    }
    for (size_t i = 0; i < len; i++) {
        s_precrash.data[s_precrash.head] = s[i];
        s_precrash.head = (s_precrash.head + 1) % PRECRASH_RING_BYTES;
        if (s_precrash.count < PRECRASH_RING_BYTES) s_precrash.count++;
    }
    /* Keep magic + size invariants consistent — the snapshot reader at
     * the start of the next boot checks both. */
    s_precrash.magic = PRECRASH_MAGIC;
}

/* Wrap IDF's panic-print routines so every character/string the crash
 * handler emits (header, register dump, "Backtrace: 0x… …", DWARF
 * unwind output, abort reason) lands in the RTC RAM ring before going
 * out the UART.
 *
 * We need to wrap BOTH:
 *   - panic_print_str — catches calls from panic_handler.c /
 *     panic_arch.c / eh_frame_parser.c (different objects, so the
 *     linker's --wrap actually intercepts them).
 *   - panic_print_char — catches the rare direct callers and the
 *     panic_print_hex/dec output that bypasses panic_print_str.
 *
 * The two would double-count the chars panic_print_str loops through
 * to panic_print_char inside panic.c — but those calls are INTRA-
 * OBJECT (same .o), which linker --wrap does NOT intercept. So in
 * practice each character lands in the ring exactly once. */
extern void __real_panic_print_char(char c);
void __wrap_panic_print_char(char c)
{
    log_capture_append_panic(&c, 1);
    __real_panic_print_char(c);
}

extern void __real_panic_print_str(const char *str);
void __wrap_panic_print_str(const char *str)
{
    if (str) {
        size_t n = 0;
        while (str[n]) n++;
        log_capture_append_panic(str, n);
    }
    __real_panic_print_str(str);
}

/* panic_print_hex / panic_print_dec call panic_print_char intra-object
 * inside panic.c, so a wrap on panic_print_char doesn't see those
 * digits — the Backtrace's "0x........" hex addresses came out as
 * empty "0x:0x". Re-format the same digits here into the ring before
 * delegating to the real one. */
extern void __real_panic_print_hex(int h);
void __wrap_panic_print_hex(int h)
{
    char buf[8];
    for (int x = 0; x < 8; x++) {
        int c = (h >> 28) & 0xf;
        buf[x] = (c < 10) ? ('0' + c) : ('a' + c - 10);
        h <<= 4;
    }
    log_capture_append_panic(buf, 8);
    __real_panic_print_hex(h);
}

extern void __real_panic_print_dec(int d);
void __wrap_panic_print_dec(int d)
{
    char buf[2];
    int n1 = d % 10;
    int n2 = d / 10;
    buf[0] = (n2 == 0) ? ' ' : (n2 + '0');
    buf[1] = n1 + '0';
    log_capture_append_panic(buf, 2);
    __real_panic_print_dec(d);
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
    /* Ring buffer on SPIRAM — 16 KB default, an internal-DRAM alloc
     * just to feed the /api/log/raw poller would be wasteful. The
     * ESP_LOG vprintf hook that writes here runs in task context
     * (not ISR), and the /api/log/raw read path is httpd-thread,
     * so SPIRAM placement is safe. Falls back to internal if SPIRAM
     * is unavailable. */
    s_buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!s_buf) s_buf = malloc(cap);
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
