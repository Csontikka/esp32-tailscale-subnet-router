/* Reset-history ring buffer — keeps the 10 most recent boots persisted to
 * NVS as a single blob (`rst_hist`) so the System tab can show a full
 * trail back from "panic two boots ago" without needing serial logs.
 * Entry [0] is always the most recent boot.
 *
 * The recorder runs once at app_main() time, right after nvs_flash_init
 * and before the rest of the subsystems come up. It:
 *   - computes the boot reason (with FLASH / ROLLBACK detection via the
 *     app SHA8 fingerprint trick, since IDF v5.3 reports UNKNOWN both
 *     for first-boot and for the esptool/PIO post-flash chip reset),
 *   - reads any prior `rst_hist` blob (with v1→v2 migration),
 *   - shifts everything down a slot,
 *   - writes the current boot into hist[0],
 *   - persists the blob.
 *
 * The matching coredump backfill — when a core dump exists from the
 * panic that triggered this boot — writes a one-line crash summary into
 * hist[0].crash via reset_history_set_current_crash(). */

#ifndef RESET_HISTORY_H_
#define RESET_HISTORY_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESET_HISTORY_MAX 10

/* Layout v4 (2026-06-03): crash 96→128. v3 had shrunk who[64]→32 +
 * crash[160]→96 to relieve a 24K NVS partition that was filling in under a
 * day; the partition is now 64K, so a 10-entry blob of 1800 B is comfortable.
 * The extra room lets the crash one-liner hold ~8 backtrace frames — enough
 * to see PAST the abort() machinery (panic_abort/esp_system_abort/abort,
 * which are the same 3 frames for every abort-based panic) to the real
 * culprit. The full panic dump still lives in the coredump partition; the
 * full frame list also rides up to telemetry via NVS.last_crash.
 * Note: a struct-size change invalidates the old blob, so reset history is
 * zeroed once on the first boot of this firmware (one-time, harmless). */
typedef struct {
    uint32_t wallclock;     /* unix epoch seconds when this entry was saved; 0 if SNTP hadn't synced at boot */
    char     reason[16];    /* "PANIC", "SW", "TASK_WDT", "FLASH", ... */
    char     who[32];       /* deliberate-restart attribution; "" for hardware resets */
    char     crash[128];    /* core-dump one-liner for PANIC/WDT rows; "" otherwise */
} reset_history_entry_t;

/* Record the current boot into hist[0]. Idempotent within a single boot
 * but you almost certainly want to call it exactly once. Reads
 * esp_reset_reason() / does the SHA8 just-flashed check internally. */
void reset_history_record_boot(void);

/* Called from the coredump→NVS block in main.c when a core dump from
 * the panic that triggered this boot is decoded. Stores the one-liner
 * into hist[0].crash and re-persists the blob. */
void reset_history_set_current_crash(const char *crash_info);

/* Read up to `max` entries into `out`, most-recent first. Returns the
 * number of valid entries (0..max). Used by /api/system and the SPA. */
int reset_history_load(reset_history_entry_t *out, int max);

/* Called from note_restart_reason() before esp_restart() so the next boot
 * can attribute the SW reset to a specific cause (e.g. "web ota",
 * "factory reset", "cli restart"). */
void reset_history_note_restart(const char *who);

#ifdef __cplusplus
}
#endif

#endif /* RESET_HISTORY_H_ */
