/* Small wrapper around the ESP-IDF NVS API for the single project
 * namespace ("tsr"). Every other module in the firmware talks to NVS
 * through these helpers so the namespace + read/write conventions live
 * in exactly one place.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace shared by every module. Keep it short — NVS imposes a
 * 15-character limit. */
#define NVS_NAMESPACE "tsr"

/* String params. nvs_param_get_str() allocates the returned buffer with
 * malloc(); caller frees. Returns NULL if the key doesn't exist. */
char       *nvs_param_get_str(const char *key);
esp_err_t   nvs_param_set_str(const char *key, const char *value);

/* Read a string into a caller-supplied buffer. out_len is in/out: pass
 * the buffer size in, get the actual stored length back (including
 * null terminator). Returns ESP_OK / ESP_ERR_NVS_NOT_FOUND. */
esp_err_t   nvs_param_get_str_buf(const char *key, char *out, size_t *out_len);

/* Integer params. The signed variant covers ints, the unsigned variant
 * covers u8/u16/u32 stored under the same key — pick the one that
 * matches what was written. */
esp_err_t   nvs_param_get_int(const char *key, int32_t *out);
esp_err_t   nvs_param_set_int(const char *key, int32_t value);
esp_err_t   nvs_param_get_u8 (const char *key, uint8_t  *out);
esp_err_t   nvs_param_set_u8 (const char *key, uint8_t   value);
esp_err_t   nvs_param_get_u16(const char *key, uint16_t *out);
esp_err_t   nvs_param_set_u16(const char *key, uint16_t  value);
esp_err_t   nvs_param_get_u32(const char *key, uint32_t *out);
esp_err_t   nvs_param_set_u32(const char *key, uint32_t  value);

/* Erase a single key. ESP_OK or ESP_ERR_NVS_NOT_FOUND. */
esp_err_t   nvs_param_erase(const char *key);

/* Erase every key in the namespace (factory-reset style). */
esp_err_t   nvs_param_erase_all(void);

#ifdef __cplusplus
}
#endif
