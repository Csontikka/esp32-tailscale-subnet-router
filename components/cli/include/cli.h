/* Serial CLI over the default UART0 console. Starts an esp_console
 * REPL with a small command set that operates on the same NVS / runtime
 * state as the web UI — no parallel storage, no shadow config.
 *
 * Spawn from app_main() after every other subsystem is initialised
 * (the handlers need nvs_params / wifi_networks / dns_relay / etc.
 * to be ready to answer). */

#ifndef CLI_H_
#define CLI_H_

#ifdef __cplusplus
extern "C" {
#endif

void cli_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CLI_H_ */
