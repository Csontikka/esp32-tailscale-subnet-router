/* Self-signed HTTPS cert generation + NVS caching.
 *
 * On first boot we have no clock, no CA chain to anchor to, and no
 * operator-provided certificate — but the SPA needs a secure context
 * to access WebCrypto for the encrypted-backup feature. We forge a
 * fresh per-device EC P-256 keypair + self-signed cert at boot, cache
 * both PEM blobs in NVS, and feed them straight to esp_https_server.
 *
 * The browser shows a one-time NET::ERR_CERT_AUTHORITY_INVALID
 * warning; once the operator clicks through, the cert is trusted for
 * the device's lifetime. factory_reset wipes the NVS entries so the
 * next boot regenerates.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch the cached cert + key, generating fresh ones on first boot
 * if NVS is empty. Both buffers are PEM-encoded and NUL-terminated;
 * caller owns the malloc'd memory and must free() both. */
esp_err_t https_cert_get_or_create(
    unsigned char **crt_pem, size_t *crt_pem_len,
    unsigned char **key_pem, size_t *key_pem_len);

/* Wipe both NVS entries — next boot regenerates a fresh keypair.
 * Hooked from factory_reset so a re-flashed device doesn't keep
 * answering with the cert of its previous owner. */
void https_cert_clear(void);

#ifdef __cplusplus
}
#endif
