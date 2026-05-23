/* Self-signed HTTPS cert generation + NVS caching.
 *
 * EC P-256 keypair + self-signed X.509 cert. Generated once at first
 * boot, cached as PEM strings in NVS under namespace "tsr" / keys
 * "https_crt" + "https_key". See https_cert.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "https_cert.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_params.h"

#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/version.h"

static const char *TAG = "https_cert";

#define NVS_KEY_CRT "https_crt"
#define NVS_KEY_KEY "https_key"

/* mbedTLS expects the validity strings as YYYYMMDDhhmmss. We have no
 * RTC at first boot, so the date is fixed at compile time. 2030-01-01
 * is plenty long enough for a router cert — the operator can wipe
 * the NVS entries via factory_reset to regenerate. */
#define CERT_NOT_BEFORE "20200101000000"
#define CERT_NOT_AFTER  "20991231235959"

#define DN_TEMPLATE "CN=%s,O=ESP32 Tailscale Subnet Router"

/* mbedTLS error → log helper. Keeps each call site to one line. */
static void log_mbedtls_err(const char *what, int rc)
{
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof buf);
    ESP_LOGE(TAG, "%s failed: -0x%04x (%s)", what, (unsigned)-rc, buf);
}

/* Forge a fresh keypair + cert and write both PEMs to NVS. Returns
 * malloc'd PEM buffers via the out params (caller owns). Anything
 * left over from the cached path is freed and replaced. */
static esp_err_t generate_and_cache(
    unsigned char **crt_pem, size_t *crt_pem_len,
    unsigned char **key_pem, size_t *key_pem_len)
{
    int rc;
    esp_err_t err = ESP_FAIL;

    mbedtls_pk_context        key;
    mbedtls_x509write_cert    crt;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  drbg;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    /* Subject CN — operator-set hostname when available, fallback to a
     * stable generic so the cert at least has *some* identifier. */
    char *hostname = nvs_param_get_str("hostname");
    const char *cn = (hostname && hostname[0]) ? hostname : "esp32-router";
    char subject[96];
    snprintf(subject, sizeof subject, DN_TEMPLATE, cn);

    const char *pers = "tsr-https-cert";
    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (rc != 0) { log_mbedtls_err("ctr_drbg_seed", rc); goto cleanup; }

    /* EC P-256 — ~10× faster generation than RSA-2048 and yields a
     * smaller cert (~600 B vs ~1.2 KB), which matters for both NVS
     * footprint and TLS handshake bytes. */
    rc = mbedtls_pk_setup(&key,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) { log_mbedtls_err("pk_setup", rc); goto cleanup; }

    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                              mbedtls_pk_ec(key),
                              mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) { log_mbedtls_err("ecp_gen_key", rc); goto cleanup; }

    /* Random 8-byte serial. MSB cleared so the DER INTEGER encoding
     * stays positive without an extra leading-zero pad. */
    uint8_t serial_raw[8];
    esp_fill_random(serial_raw, sizeof serial_raw);
    serial_raw[0] &= 0x7F;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    /* mbedTLS 3.x replaced the mpi-taking set_serial() with set_serial_raw()
     * which takes the serial as a big-endian byte string. The old name is
     * gone, not just deprecated, so we go straight to the raw form. */
    rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial_raw, sizeof serial_raw);
    if (rc != 0) { log_mbedtls_err("set_serial_raw", rc); goto cleanup; }

    rc = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (rc != 0) { log_mbedtls_err("set_subject_name", rc); goto cleanup; }
    rc = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    if (rc != 0) { log_mbedtls_err("set_issuer_name", rc); goto cleanup; }

    rc = mbedtls_x509write_crt_set_validity(&crt,
                                              CERT_NOT_BEFORE, CERT_NOT_AFTER);
    if (rc != 0) { log_mbedtls_err("set_validity", rc); goto cleanup; }

    rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (rc != 0) { log_mbedtls_err("set_basic_constraints", rc); goto cleanup; }
    rc = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    if (rc != 0) { log_mbedtls_err("set_subject_key_id", rc); goto cleanup; }
    rc = mbedtls_x509write_crt_set_authority_key_identifier(&crt);
    if (rc != 0) { log_mbedtls_err("set_authority_key_id", rc); goto cleanup; }

    /* PEM out — 2 KB stack buf is twice what EC P-256 needs. */
    unsigned char tmp[2048];
    rc = mbedtls_x509write_crt_pem(&crt, tmp, sizeof tmp,
                                     mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) { log_mbedtls_err("x509write_crt_pem", rc); goto cleanup; }
    size_t crt_len = strlen((const char *)tmp);
    unsigned char *crt_buf = malloc(crt_len + 1);
    if (!crt_buf) { ESP_LOGE(TAG, "malloc(crt_buf %zu) failed", crt_len + 1); goto cleanup; }
    memcpy(crt_buf, tmp, crt_len + 1);

    rc = mbedtls_pk_write_key_pem(&key, tmp, sizeof tmp);
    if (rc != 0) {
        log_mbedtls_err("pk_write_key_pem", rc);
        free(crt_buf);
        goto cleanup;
    }
    size_t key_len = strlen((const char *)tmp);
    unsigned char *key_buf = malloc(key_len + 1);
    if (!key_buf) {
        ESP_LOGE(TAG, "malloc(key_buf %zu) failed", key_len + 1);
        free(crt_buf);
        goto cleanup;
    }
    memcpy(key_buf, tmp, key_len + 1);

    /* Persist to NVS so subsequent boots load instantly without
     * the ~1-2 s EC keygen + cert-build delay. */
    esp_err_t e1 = nvs_param_set_str(NVS_KEY_CRT, (const char *)crt_buf);
    esp_err_t e2 = nvs_param_set_str(NVS_KEY_KEY, (const char *)key_buf);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "NVS persist failed crt=%s key=%s",
                  esp_err_to_name(e1), esp_err_to_name(e2));
        /* Non-fatal — we still return the in-memory cert so the
         * server starts; the next boot just regenerates. */
    }

    *crt_pem     = crt_buf;
    *crt_pem_len = crt_len;
    *key_pem     = key_buf;
    *key_pem_len = key_len;
    ESP_LOGI(TAG, "generated EC P-256 self-signed cert: CN=%s "
                  "(crt=%zu B key=%zu B)", cn, crt_len, key_len);
    err = ESP_OK;

cleanup:
    free(hostname);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return err;
}

esp_err_t https_cert_get_or_create(
    unsigned char **crt_pem, size_t *crt_pem_len,
    unsigned char **key_pem, size_t *key_pem_len)
{
    if (!crt_pem || !crt_pem_len || !key_pem || !key_pem_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *crt_pem = NULL; *key_pem = NULL;
    *crt_pem_len = 0; *key_pem_len = 0;

    char *crt = nvs_param_get_str(NVS_KEY_CRT);
    char *key = nvs_param_get_str(NVS_KEY_KEY);
    if (crt && crt[0] && key && key[0]) {
        *crt_pem     = (unsigned char *)crt;
        *crt_pem_len = strlen(crt);
        *key_pem     = (unsigned char *)key;
        *key_pem_len = strlen(key);
        ESP_LOGI(TAG, "loaded cached HTTPS cert from NVS "
                      "(crt=%zu B key=%zu B)", *crt_pem_len, *key_pem_len);
        return ESP_OK;
    }
    /* Either side missing → wipe both to keep them consistent and
     * regenerate. */
    free(crt); free(key);
    ESP_LOGW(TAG, "no cached HTTPS cert in NVS, generating fresh");
    return generate_and_cache(crt_pem, crt_pem_len, key_pem, key_pem_len);
}

void https_cert_clear(void)
{
    nvs_param_set_str(NVS_KEY_CRT, "");
    nvs_param_set_str(NVS_KEY_KEY, "");
    ESP_LOGI(TAG, "cached HTTPS cert + key wiped from NVS");
}
