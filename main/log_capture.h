/* In-memory ring-buffer capture of ESP_LOG output, for the /log web
 * page. Installs an esp_log_set_vprintf() hook that forks each line to
 * both UART (original sink) and a circular buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the ring buffer and install the log hook. Safe to call
 * exactly once at boot. `bytes` rounds up to the next 1 KB; pass 0 for
 * the default 16 KB. Returns ESP_OK on success, ESP_ERR_NO_MEM if the
 * allocation fails. */
esp_err_t log_capture_init(size_t bytes);

/* Copy the buffer contents into `out` in chronological order (oldest
 * first). Returns the number of bytes written (always <= out_size-1, a
 * trailing NUL is added). If the buffer wrapped, the returned text
 * starts at the oldest preserved byte — not necessarily a line
 * boundary. */
size_t log_capture_read(char *out, size_t out_size);

/* Discard all captured bytes. */
void log_capture_clear(void);

/* Total bytes currently buffered (0..capacity). */
size_t log_capture_size(void);

/* Allocated buffer size (always a multiple of 1 KB). */
size_t log_capture_capacity(void);

/* Monotone counter of total bytes ever appended (does NOT wrap on the
 * ring boundary — used by /log/raw clients to ask for "everything since
 * I last polled"). */
uint64_t log_capture_seq(void);

/* Copy the slice of the buffer with absolute offsets > `since_seq` into
 * `out`. On return, `*new_seq` is set to log_capture_seq() at the moment
 * of read so the caller can use it for the next poll. If `since_seq`
 * sits before the oldest preserved byte (i.e. the client fell behind
 * and the ring already wrapped past their cursor), we return the full
 * live buffer and `*new_seq` is the current head — the client should
 * treat that as a re-sync rather than appending. Returns the byte
 * count written. */
size_t log_capture_read_since(uint64_t since_seq, char *out, size_t out_size,
                              uint64_t *new_seq);

/* Pre-crash snapshot — the contents of the RTC slow-RAM ring as it was
 * at the moment the *previous* boot died. Survives soft reset / PANIC /
 * watchdog; lost on brown-out and cold power-on. Snapshot is taken
 * once at boot, before the live hook starts overwriting the RTC ring,
 * so reading it never races the running capture. */
bool   log_capture_have_precrash(void);
size_t log_capture_precrash_size(void);
size_t log_capture_read_precrash(char *out, size_t out_size);
void   log_capture_clear_precrash(void);

/* Lock-free append straight into the RTC pre-crash ring. Safe to call
 * from inside the panic handler / interrupt context — no critical-
 * section, no logging, no flash-resident calls. Wrapped panic_print_char
 * uses this to mirror the IDF crash dump (Guru Meditation header,
 * register snapshot, backtrace) into the same RAM buffer the regular
 * ESP_LOG capture uses, so the next boot's /api/log/precrash returns
 * both the heartbeat tail AND the actual panic output. */
void log_capture_append_panic(const char *s, size_t len);

#ifdef __cplusplus
}
#endif
