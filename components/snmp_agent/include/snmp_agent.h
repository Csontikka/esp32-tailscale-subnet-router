/* SNMP agent component — lwIP built-in SNMPv1/v2c, read-only.
 * Binds to 0.0.0.0:161 so it is reachable on all interfaces including
 * the Tailscale netif. MIB-II (interfaces, IP, UDP) plus a private
 * enterprise subtree with free-heap gauges.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* NVS key names shared between snmp_agent.c (init) and web_ui.c (POST
 * handler, which does NVS writes via the error-tracking wrappers). */
#define SNMP_NVS_KEY_EN       "snmp_en"
#define SNMP_NVS_KEY_COMM     "snmp_comm"
#define SNMP_NVS_KEY_NAME     "snmp_name"
#define SNMP_NVS_KEY_CONTACT  "snmp_contact"
#define SNMP_NVS_KEY_LOCATION "snmp_location"

/* Initialise from NVS and start the agent if enabled.
 * Call once from app_main, after nvs_flash_init and before web_ui_init. */
void snmp_agent_init(void);

/* Apply a live config change.  NVS persistence is the caller's
 * responsibility (web_ui POST handler uses nvs_save_* wrappers). */
void snmp_agent_apply_live(bool enabled,
                           const char *community,
                           const char *sys_name,
                           const char *sys_contact,
                           const char *sys_location);

/* State queries — safe to call from any task. */
bool snmp_agent_is_enabled(void);
bool snmp_agent_is_running(void);

/* Fill caller-provided buffers with the current config + live state.
 * All pointer arguments must be non-NULL. */
void snmp_agent_get_config(bool *enabled_out, bool *running_out,
                           char *community,    size_t comm_sz,
                           char *sys_name,     size_t name_sz,
                           char *sys_contact,  size_t cont_sz,
                           char *sys_location, size_t loc_sz);

/* Die temperature in °C. Returns false if the sensor is unavailable.
 *
 * The ESP32-S3 has exactly one on-die thermal sensor and
 * temperature_sensor_install() refuses a second owner, so this agent
 * installs it once at init (regardless of whether SNMP is enabled) and
 * everything else in the firmware reads it through here. web_ui.c used to
 * lazy-install its own and, losing the race to snmp_agent_init(), reported
 * no temperature at all while logging "Already installed" on every status
 * poll. Safe to call from any task. */
bool snmp_agent_chip_temp_c(float *out_c);
