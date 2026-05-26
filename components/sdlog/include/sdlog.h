/* SD flight-recorder - persistent on-device log to a microSD card.
 *
 * Mirrors the syslog_client architecture: a vprintf hook formats each
 * ESP_LOG line into a FreeRTOS queue; a dedicated writer task fwrites
 * them to a rotating set of files on a FAT-mounted microSD card.
 * Serial console output is always preserved (the hook chains to the
 * previously-installed vprintf, i.e. syslog's, which chains to UART).
 *
 * The card survives reboots, so the log is the place to look after a
 * silent control-plane wedge: a fresh boot-marker file is created on
 * every boot, and (Phase B) a periodic SNAP line records task states +
 * DERP heartbeat age even while coord/derp are blocked.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Verbosity levels stored in NVS (sdlog_verb). */
#define SDLOG_VERB_ALL   0   /* every ESP_LOG line              */
#define SDLOG_VERB_WARN  1   /* WARN + ERROR only               */

/* Length of the short 8.3 filename strings we hand back to callers,
 * e.g. "00000001.LOG" (12 chars + NUL). */
#define SDLOG_NAME_MAX   16

/* Snapshot of the recorder state for the /api/sdlog status endpoint. */
typedef struct {
    bool     present;        /* microSD mounted OK                       */
    bool     enabled;        /* recorder active (writer task running)    */
    uint8_t  verbosity;      /* SDLOG_VERB_*                             */
    uint32_t card_mb;        /* formatted card capacity, MiB             */
    uint32_t free_mb;        /* free space, MiB                          */
    char     cur_file[SDLOG_NAME_MAX]; /* basename of the active file    */
    uint32_t file_count;     /* number of *.LOG files in the log dir     */
    uint32_t dropped;        /* lines dropped because the queue was full */
    uint64_t bytes_written;  /* total bytes fwritten this boot           */
} sdlog_status_t;

/* One entry of the file listing (GET /sdlog/list). */
typedef struct {
    char     name[SDLOG_NAME_MAX];
    uint32_t size;           /* bytes  */
    uint32_t mtime;          /* unix seconds, 0 if unknown */
} sdlog_file_info_t;

/* Chunk callback used to stream a file out of sdlog without exposing the
 * internal FAT lock. Return ESP_OK to continue, anything else to abort. */
typedef esp_err_t (*sdlog_chunk_cb)(void *ctx, const char *buf, size_t len);

/**
 * Mount the microSD card, load NVS config, and start the writer task if
 * the recorder was previously enabled. Safe to call when no card is
 * present — the feature simply stays dark (present=false), no crash.
 *
 * Call AFTER syslog_init() so the vprintf hook chain stays:
 *   sdlog -> syslog -> UART.
 */
esp_err_t sdlog_init(void);

/** True if the microSD card mounted successfully at init. */
bool sdlog_card_present(void);

/** True if the recorder is currently enabled (writer task running). */
bool sdlog_is_enabled(void);

/**
 * Enable the recorder at the given verbosity (SDLOG_VERB_*). Persists to
 * NVS, creates the queue + writer task, and starts a fresh boot file.
 * No-op returning ESP_ERR_INVALID_STATE if no card is present.
 */
esp_err_t sdlog_enable(uint8_t verbosity);

/** Disable the recorder. Stops the writer task and persists the flag. */
esp_err_t sdlog_disable(void);

/** Fill *out with the current recorder status. */
void sdlog_get_status(sdlog_status_t *out);

/**
 * List up to max_entries *.LOG files (newest first). Returns the number
 * filled, or a negative esp_err_t on failure / no card.
 */
int sdlog_list_files(sdlog_file_info_t *out, int max_entries);

/**
 * Stream the named log file out via cb (called with successive chunks).
 * name must be a bare basename living in the log directory — any '/',
 * '\\' or ".." is rejected. Returns ESP_OK on success.
 */
esp_err_t sdlog_read_file(const char *name, sdlog_chunk_cb cb, void *ctx);

/**
 * Copy the last n_lines of the named file into buf (NUL-terminated).
 * *out_len receives the byte count written (excluding the NUL).
 */
esp_err_t sdlog_tail_file(const char *name, int n_lines,
                          char *buf, size_t buf_size, size_t *out_len);

/** Delete every *.LOG file in the log directory. */
esp_err_t sdlog_erase_all(void);

/**
 * Hand sdlog the microlink handle so the writer task can emit the Phase B
 * SNAP health line (task states + DERP heartbeat age). Pass an opaque
 * microlink_t* (void* keeps this header free of a microlink dependency).
 * Call once after microlink_init; until then SNAP omits the tailnet fields.
 */
void sdlog_set_microlink(void *ml);

#ifdef __cplusplus
}
#endif
