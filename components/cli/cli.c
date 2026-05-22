/* Serial REPL on UART0. Clean-room implementation — does not use any
 * code from the upstream esp32_nat_router CLI; commands here act on
 * the same JSON-API contract the SPA already exposes, just driven by
 * argtable3 string handlers.
 *
 * SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cli.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "argtable3/argtable3.h"

#include "web_password.h"
#include "dns_relay.h"

static const char *TAG = "cli";

/* The CLI handlers all operate against the project NVS namespace. We
 * deliberately don't reuse the main/ nvs_params helpers to keep this
 * component standalone (so it can also link in trimmed test builds). */
#define CLI_NVS_NS "tsr"

/* ===== helpers ===== */

static esp_err_t nvs_set_str_ns(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_get_str_ns(const char *key, char *out, size_t out_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t need = out_size;
    err = nvs_get_str(h, key, out, &need);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_i32_ns(const char *key, int32_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ===== handlers ===== */

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t up_us = esp_timer_get_time();
    uint32_t up_s = (uint32_t)(up_us / 1000000ULL);

    const esp_app_desc_t *desc = esp_app_get_description();
    printf("firmware    : %s built %s %s (%s)\n",
           desc ? desc->version    : "?",
           desc ? desc->date       : "?",
           desc ? desc->time       : "?",
           desc ? desc->idf_ver    : "?");
    printf("uptime      : %lu s (%lu h %lu m)\n",
           (unsigned long)up_s, (unsigned long)(up_s / 3600),
           (unsigned long)((up_s / 60) % 60));
    printf("free heap   : %u B\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    printf("min heap    : %u B\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        char ipbuf[16];
        esp_ip4addr_ntoa(&ip.ip, ipbuf, sizeof ipbuf);
        printf("STA IP      : %s\n", ip.ip.addr ? ipbuf : "(no link)");
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
        char ipbuf[16];
        esp_ip4addr_ntoa(&ip.ip, ipbuf, sizeof ipbuf);
        printf("AP IP       : %s\n", ipbuf);
    }
    wifi_ap_record_t apr = {0};
    if (esp_wifi_sta_get_ap_info(&apr) == ESP_OK) {
        printf("STA SSID    : %s  rssi=%d\n", (char *)apr.ssid, (int)apr.rssi);
    }
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        printf("AP clients  : %d\n", (int)sta_list.num);
    }
    printf("web password: %s\n", is_web_password_set() ? "set" : "(none)");
    printf("DNS relay   : %s%s\n",
           dns_relay_is_enabled() ? "ON"  : "OFF",
           dns_relay_is_healthy() ? " (healthy)" : "");
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Rebooting in 200 ms...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0; /* not reached */
}

/* Deliberately trigger a panic — only useful for testing the
 * coredump + pre-crash-log capture chain. Guarded by --confirm so a
 * stray paste can't crash the operator's device. */
static struct { struct arg_lit *confirm; struct arg_end *end; } s_cr;
static int cmd_crash(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_cr) != 0 || s_cr.confirm->count == 0) {
        printf("Refusing — type:  crash --confirm  to abort() the device.\n");
        return 1;
    }
    printf("Aborting in 100 ms — pre-crash log + coredump should land.\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();
    return 0;
}

/* factory_reset requires --confirm so a stray paste of an unrelated
 * `factory_reset` line into a pio device monitor session can't brick
 * the operator's configuration. Matches the SPA endpoint, which gates
 * the same destructive action on an explicit JSON confirm field. */
static struct { struct arg_lit *confirm; struct arg_end *end; } s_fr;
static int cmd_factory_reset(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_fr) != 0 || s_fr.confirm->count == 0) {
        printf("Refusing to erase NVS without --confirm.\n");
        printf("Type:  factory_reset --confirm\n");
        return 1;
    }
    printf("Erasing NVS and rebooting...\n");
    fflush(stdout);
    nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

/* ----- web_password ----- */
static struct { struct arg_str *pw; struct arg_end *end; } s_pw;
static int cmd_web_password(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_pw) != 0) {
        arg_print_errors(stderr, s_pw.end, argv[0]);
        return 1;
    }
    const char *pw = (s_pw.pw->count > 0) ? s_pw.pw->sval[0] : "";
    esp_err_t err = set_web_password_hashed(pw);
    if (err != ESP_OK) {
        printf("ERROR: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("web password %s\n", (pw && pw[0]) ? "set" : "cleared");
    return 0;
}

/* ----- dns_relay ----- */
static struct { struct arg_str *mode; struct arg_end *end; } s_dr;
static int cmd_dns_relay(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_dr) != 0) {
        arg_print_errors(stderr, s_dr.end, argv[0]);
        return 1;
    }
    const char *m = (s_dr.mode->count > 0) ? s_dr.mode->sval[0] : "status";
    if (!strcmp(m, "on"))       dns_relay_set_enabled(true);
    else if (!strcmp(m, "off")) dns_relay_set_enabled(false);
    else if (strcmp(m, "status") != 0) {
        printf("usage: dns_relay <on|off|status>\n"); return 1;
    }
    printf("dns_relay: enabled=%s healthy=%s\n",
           dns_relay_is_enabled() ? "yes" : "no",
           dns_relay_is_healthy() ? "yes" : "no");
    return 0;
}

/* ----- tailscale ----- */
static struct { struct arg_str *mode; struct arg_end *end; } s_ts;
static int cmd_tailscale(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_ts) != 0) {
        arg_print_errors(stderr, s_ts.end, argv[0]);
        return 1;
    }
    const char *m = (s_ts.mode->count > 0) ? s_ts.mode->sval[0] : "status";
    int32_t cur = 0;
    nvs_handle_t h;
    if (nvs_open(CLI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "ts_enabled", &cur);
        nvs_close(h);
    }
    if (!strcmp(m, "on") || !strcmp(m, "off")) {
        int32_t v = !strcmp(m, "on") ? 1 : 0;
        if (v != cur) {
            nvs_set_i32_ns("ts_enabled", v);
            printf("ts_enabled → %ld — reboot for it to take effect\n", (long)v);
        } else {
            printf("ts_enabled already %ld\n", (long)v);
        }
        return 0;
    } else if (!strcmp(m, "status")) {
        printf("ts_enabled (NVS) : %ld\n", (long)cur);
        return 0;
    }
    printf("usage: tailscale <on|off|status>\n");
    return 1;
}

/* ----- nvs_get / nvs_set (string-only diag helpers) ----- */
static struct { struct arg_str *key; struct arg_end *end; } s_nvg;
static int cmd_nvs_get(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_nvg) != 0) {
        arg_print_errors(stderr, s_nvg.end, argv[0]); return 1;
    }
    char buf[256] = {0};
    esp_err_t err = nvs_get_str_ns(s_nvg.key->sval[0], buf, sizeof buf);
    if (err == ESP_OK) {
        printf("%s = \"%s\"\n", s_nvg.key->sval[0], buf);
    } else {
        printf("%s: %s\n", s_nvg.key->sval[0], esp_err_to_name(err));
    }
    return 0;
}

static struct { struct arg_str *key; struct arg_str *value; struct arg_end *end; } s_nvs;
static int cmd_nvs_set(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_nvs) != 0) {
        arg_print_errors(stderr, s_nvs.end, argv[0]); return 1;
    }
    esp_err_t err = nvs_set_str_ns(s_nvs.key->sval[0], s_nvs.value->sval[0]);
    if (err == ESP_OK) printf("OK\n");
    else               printf("ERROR: %s\n", esp_err_to_name(err));
    return 0;
}

/* ----- set_sta / set_ap (legacy single-network/AP helpers) ----- */
static struct { struct arg_str *ssid; struct arg_str *pw; struct arg_end *end; } s_sta, s_ap;

static int cmd_set_sta(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_sta) != 0) {
        arg_print_errors(stderr, s_sta.end, argv[0]); return 1;
    }
    /* Write into the legacy keys; wifi_networks_init() migrates them
     * into slot 0 of the multi-network blob on next boot. We don't
     * link wifi_networks directly to keep this component standalone. */
    esp_err_t e1 = nvs_set_str_ns("ssid",   s_sta.ssid->sval[0]);
    esp_err_t e2 = ESP_OK;
    if (s_sta.pw->count > 0) e2 = nvs_set_str_ns("passwd", s_sta.pw->sval[0]);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        printf("ERROR: %s\n", esp_err_to_name(e1 != ESP_OK ? e1 : e2));
        return 1;
    }
    printf("STA creds saved — reboot for them to take effect\n");
    return 0;
}

static int cmd_set_ap(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_ap) != 0) {
        arg_print_errors(stderr, s_ap.end, argv[0]); return 1;
    }
    esp_err_t e1 = nvs_set_str_ns("ap_ssid",  s_ap.ssid->sval[0]);
    esp_err_t e2 = ESP_OK;
    if (s_ap.pw->count > 0) e2 = nvs_set_str_ns("ap_passwd", s_ap.pw->sval[0]);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        printf("ERROR: %s\n", esp_err_to_name(e1 != ESP_OK ? e1 : e2));
        return 1;
    }
    printf("AP creds saved — reboot for them to take effect\n");
    return 0;
}

/* ===== registration ===== */

static void register_one(const char *name, const char *help, const char *hint,
                         esp_console_cmd_func_t func, void *argtable)
{
    esp_console_cmd_t cmd = {
        .command  = name,
        .help     = help,
        .hint     = hint,
        .func     = func,
        .argtable = argtable,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt          = "tsr> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed — CLI disabled");
        return;
    }

    /* Built-in `help` is registered by esp_console_register_help_command. */
    esp_console_register_help_command();

    register_one("status",        "Print device runtime status",          NULL, cmd_status, NULL);
    register_one("restart",       "Reboot the device",                    NULL, cmd_restart, NULL);

    s_fr.confirm = arg_lit0(NULL, "confirm", "REQUIRED — guards against accidental erase");
    s_fr.end     = arg_end(2);
    register_one("factory_reset", "Erase NVS and reboot (needs --confirm)", NULL, cmd_factory_reset, &s_fr);

    s_cr.confirm = arg_lit0(NULL, "confirm", "REQUIRED — abort() the device on purpose");
    s_cr.end     = arg_end(2);
    register_one("crash",         "Deliberately panic for testing (needs --confirm)", NULL, cmd_crash, &s_cr);

    s_pw.pw  = arg_str0(NULL, NULL, "<password>", "new password (empty to disable)");
    s_pw.end = arg_end(2);
    register_one("web_password",  "Set or clear the web UI password",     NULL, cmd_web_password, &s_pw);

    s_dr.mode = arg_str0(NULL, NULL, "<on|off|status>", "default: status");
    s_dr.end  = arg_end(2);
    register_one("dns_relay",     "Toggle the in-router DNS forwarder",   NULL, cmd_dns_relay, &s_dr);

    s_ts.mode = arg_str0(NULL, NULL, "<on|off|status>", "default: status — reboot to apply");
    s_ts.end  = arg_end(2);
    register_one("tailscale",     "Enable / disable the Tailscale link",  NULL, cmd_tailscale, &s_ts);

    s_sta.ssid = arg_str1(NULL, NULL, "<ssid>",         NULL);
    s_sta.pw   = arg_str0(NULL, NULL, "<password>",     NULL);
    s_sta.end  = arg_end(2);
    register_one("set_sta",       "Set STA uplink slot 0 credentials",    NULL, cmd_set_sta, &s_sta);

    s_ap.ssid = arg_str1(NULL, NULL, "<ssid>",          NULL);
    s_ap.pw   = arg_str0(NULL, NULL, "<password>",      NULL);
    s_ap.end  = arg_end(2);
    register_one("set_ap",        "Set AP-side SSID / password",          NULL, cmd_set_ap, &s_ap);

    s_nvg.key = arg_str1(NULL, NULL, "<key>", NULL);
    s_nvg.end = arg_end(2);
    register_one("nvs_get",       "Read a string value from the tsr NVS", NULL, cmd_nvs_get, &s_nvg);

    s_nvs.key   = arg_str1(NULL, NULL, "<key>",   NULL);
    s_nvs.value = arg_str1(NULL, NULL, "<value>", NULL);
    s_nvs.end   = arg_end(2);
    register_one("nvs_set",       "Write a string value to the tsr NVS",  NULL, cmd_nvs_set, &s_nvs);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "serial REPL ready on UART0 (115200 8N1) — type 'help'");
}
