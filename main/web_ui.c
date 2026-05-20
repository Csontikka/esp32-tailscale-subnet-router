/* Single-page web UI server.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "web_ui.h"

static const char *TAG = "web_ui";

/* index.html is generated as a C source by main/CMakeLists.txt
 * (file(READ HEX) → const char index_html_start[]). The PIO build
 * wrapper can't be trusted to assemble .S files emitted by ESP-IDF's
 * EMBED_TXTFILES / target_add_binary_data — see the bug write-up in
 * main/CMakeLists.txt above the file(READ ...) call. */
extern const char   index_html_start[];
extern const size_t index_html_len;

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_len);
}

static const httpd_uri_t uri_index = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = index_handler,
    .user_ctx = NULL,
};

void web_ui_init(void)
{
    static httpd_handle_t server = NULL;
    if (server) return;

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.max_uri_handlers = 16;
    config.stack_size       = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_register_uri_handler(server, &uri_index);
    ESP_LOGI(TAG, "web UI listening on :%d", config.server_port);
}
