/* Web interface password — set, verify, query.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "web_password.h"

/* Must match the NVS_NAMESPACE define in main/nvs_params.h. Duplicated
 * here so web_password can live as its own component and be required
 * by both main and remote_console without a cycle. */
#define PW_NVS_NAMESPACE "tsr"
#define PW_NVS_KEY       "web_password"
#define PW_SALT_LEN      16
#define PW_HASH_LEN      32  /* SHA-256 output */

/* Local helpers — raw NVS instead of nvs_params.h to avoid pulling a
 * cross-component dep on main. */
static char *nvs_dup_str(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(h);
        return NULL;
    }
    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return NULL; }
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        free(buf);
        nvs_close(h);
        return NULL;
    }
    nvs_close(h);
    return buf;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void bytes_to_hex(const uint8_t *src, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", src[i]);
    }
    out[len * 2] = '\0';
}

static int hex_to_bytes(const char *src, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        if (sscanf(src + i * 2, "%2x", &b) != 1) return -1;
        dst[i] = (uint8_t)b;
    }
    return 0;
}

static void compute_hash(const uint8_t *salt, size_t salt_len,
                         const char *plaintext, uint8_t *hash_out)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, salt_len);
    mbedtls_sha256_update(&ctx, (const uint8_t *)plaintext, strlen(plaintext));
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
}

bool is_web_password_set(void)
{
    char *s = nvs_dup_str(PW_NVS_KEY);
    if (!s) return false;
    bool set = (s[0] != '\0');
    free(s);
    return set;
}

bool verify_web_password(const char *plaintext)
{
    char *stored = nvs_dup_str(PW_NVS_KEY);
    if (!stored) return false;

    /* Expected format: "salt_hex:hash_hex" — 32 + 1 + 64 chars. */
    char *colon = strchr(stored, ':');
    bool ok = false;
    if (colon
        && (colon - stored) == PW_SALT_LEN * 2
        && strlen(colon + 1) == PW_HASH_LEN * 2)
    {
        uint8_t salt[PW_SALT_LEN], stored_hash[PW_HASH_LEN], computed[PW_HASH_LEN];
        if (hex_to_bytes(stored, salt, PW_SALT_LEN) == 0
            && hex_to_bytes(colon + 1, stored_hash, PW_HASH_LEN) == 0)
        {
            compute_hash(salt, PW_SALT_LEN, plaintext, computed);
            volatile int diff = 0;
            for (int i = 0; i < PW_HASH_LEN; i++) {
                diff |= stored_hash[i] ^ computed[i];
            }
            ok = (diff == 0);
        }
    }

    free(stored);
    return ok;
}

esp_err_t set_web_password_hashed(const char *plaintext)
{
    if (plaintext[0] == '\0') {
        return nvs_write_str(PW_NVS_KEY, "");
    }

    uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
    esp_fill_random(salt, PW_SALT_LEN);
    compute_hash(salt, PW_SALT_LEN, plaintext, hash);

    char buf[PW_SALT_LEN * 2 + 1 + PW_HASH_LEN * 2 + 1];
    bytes_to_hex(salt, PW_SALT_LEN, buf);
    buf[PW_SALT_LEN * 2] = ':';
    bytes_to_hex(hash, PW_HASH_LEN, buf + PW_SALT_LEN * 2 + 1);

    return nvs_write_str(PW_NVS_KEY, buf);
}
