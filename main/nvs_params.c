/* See nvs_params.h. Boilerplate that opens the project namespace and
 * forwards to the underlying ESP-IDF NVS calls.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_params.h"

static const char *TAG = "nvs_params";

static esp_err_t open_handle(nvs_open_mode_t mode, nvs_handle_t *out)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, mode, out);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
    }
    return err;
}

char *nvs_param_get_str(const char *key)
{
    nvs_handle_t h;
    if (open_handle(NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return NULL;
    }
    char *buf = malloc(len);
    if (!buf) {
        nvs_close(h);
        return NULL;
    }
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

esp_err_t nvs_param_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_handle(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_param_get_str_buf(const char *key, char *out, size_t *out_len)
{
    if (!out || !out_len || *out_len == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = open_handle(NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, out, out_len);
    nvs_close(h);
    return err;
}

#define NVS_PARAM_INT_FN(suffix, type, getter, setter)                   \
    esp_err_t nvs_param_get_##suffix(const char *key, type *out)         \
    {                                                                    \
        if (!out) return ESP_ERR_INVALID_ARG;                            \
        nvs_handle_t h;                                                  \
        esp_err_t err = open_handle(NVS_READONLY, &h);                   \
        if (err != ESP_OK) return err;                                   \
        err = getter(h, key, out);                                       \
        nvs_close(h);                                                    \
        return err;                                                      \
    }                                                                    \
    esp_err_t nvs_param_set_##suffix(const char *key, type value)        \
    {                                                                    \
        nvs_handle_t h;                                                  \
        esp_err_t err = open_handle(NVS_READWRITE, &h);                  \
        if (err != ESP_OK) return err;                                   \
        err = setter(h, key, value);                                     \
        if (err == ESP_OK) err = nvs_commit(h);                          \
        nvs_close(h);                                                    \
        return err;                                                      \
    }

NVS_PARAM_INT_FN(int, int32_t,  nvs_get_i32, nvs_set_i32)
NVS_PARAM_INT_FN(u8,  uint8_t,  nvs_get_u8,  nvs_set_u8)
NVS_PARAM_INT_FN(u16, uint16_t, nvs_get_u16, nvs_set_u16)
NVS_PARAM_INT_FN(u32, uint32_t, nvs_get_u32, nvs_set_u32)

esp_err_t nvs_param_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = open_handle(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t nvs_param_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = open_handle(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
