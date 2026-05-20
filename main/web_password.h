/* Web interface password — set, verify, query.
 *
 * Storage: NVS key "web_password" under the project namespace, formatted
 * as "salt_hex:hash_hex" (16-byte salt + SHA-256 of salt||plaintext).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True if a non-empty password is stored. */
bool      is_web_password_set(void);

/* Constant-time verify against the stored hash. */
bool      verify_web_password(const char *plaintext);

/* Salt + hash + persist. Empty string disables protection. */
esp_err_t set_web_password_hashed(const char *plaintext);

#ifdef __cplusplus
}
#endif
