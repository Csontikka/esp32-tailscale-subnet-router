# Generate index_html_data.c from index.html at configure time. We can't
# use EMBED_TXTFILES / target_add_binary_data because PIO's SCons wrapper
# double-prefixes the .S output path, breaking the assembly step. CMake's
# file(READ HEX) + regex sidesteps that — the result is plain C the PIO
# wrapper handles normally.
#
# Written under main/ rather than ${CMAKE_CURRENT_BINARY_DIR} because the
# latter is empty during PIO's first configure pass and ends up dropping
# the file in the project root. The generated file is in .gitignore.
file(READ "${CMAKE_CURRENT_LIST_DIR}/index.html" _INDEX_HTML_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," _INDEX_HTML_BYTES "${_INDEX_HTML_HEX}")
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/index_html_data.c"
"/* GENERATED FILE — see main/CMakeLists.txt. Do not edit by hand. */
#include <stddef.h>
const char index_html_start[] = { ${_INDEX_HTML_BYTES} 0 };
const size_t index_html_len = sizeof(index_html_start) - 1;
")

idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                            "tailscale_manager.c"
                            "tailscale_mtu.c"
                            "telemetry.c"
                            "lwip_route_hook.c"
                            "web_ui.c"
                            "web_password.c"
                            "netif_hooks.c"
                            "index_html_data.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer
                                  lwip microlink
                                  esp_http_client esp-tls mbedtls
                                  esp_http_server json esp_app_format
                                  acl
                    INCLUDE_DIRS ".")

# Phase 1.9k: wrap ip_napt_forward to skip NAPT for tailnet CGNAT
# (100.64/10) destinations. Otherwise NAPT rewrites the AP-client
# source IP to the WG netif's CGNAT address and breaks the TCP
# 4-tuple at the tailnet peer.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ip_napt_forward")

# Phase 1.9l: wrap ip4_route_src_hook to add CGNAT-→-WG routing and
# the exit-node LAN-bypass + self-traffic fallback on top of the
# ESP-IDF default. The default symbol lives in
# components/lwip/port/hooks/lwip_default_hooks.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ip4_route_src_hook")
