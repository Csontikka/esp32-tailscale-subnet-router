# Generate index_html_data.c by gzipping main/index.html at configure
# time. We can't use EMBED_TXTFILES / target_add_binary_data because
# PIO's SCons wrapper double-prefixes the .S output path, breaking the
# assembly step. A helper Python script (gen_index_html_gz.py) does the
# gzip + hex-array emission — Python ships with PIO's toolchain so we
# lose no cross-platform support.
#
# Written under main/ rather than ${CMAKE_CURRENT_BINARY_DIR} because
# the latter is empty during PIO's first configure pass and ends up
# dropping the file in the project root. The generated file is in
# .gitignore.
#
# Tell CMake to re-run the helper whenever index.html OR the helper
# itself changes — without this the device would keep serving a stale
# SPA blob.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/index.html"
             "${CMAKE_CURRENT_LIST_DIR}/gen_index_html_gz.py")
find_package(Python3 REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_LIST_DIR}/gen_index_html_gz.py"
            "${CMAKE_CURRENT_LIST_DIR}/index.html"
            "${CMAKE_CURRENT_LIST_DIR}/index_html_data.c"
    RESULT_VARIABLE _gz_rc
    OUTPUT_VARIABLE _gz_out
    ERROR_VARIABLE  _gz_err)
if(NOT _gz_rc EQUAL 0)
    message(FATAL_ERROR
            "gen_index_html_gz.py failed (rc=${_gz_rc}):\n${_gz_out}${_gz_err}")
endif()
message(STATUS "index.html: ${_gz_out}")

idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                            "tailscale_manager.c"
                            "tailscale_mtu.c"
                            "telemetry.c"
                            "lwip_route_hook.c"
                            "web_ui.c"
                            "netif_hooks.c"
                            "wifi_networks.c"
                            "dhcp_reservations.c"
                            "portmap.c"
                            "mac_deny.c"
                            "reset_history.c"
                            "ota.c"
                            "index_html_data.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer
                                  lwip microlink wpa_supplicant
                                  esp_http_client esp-tls mbedtls
                                  esp_http_server json esp_app_format
                                  acl syslog sdlog pcap_capture remote_console
                                  web_password net_diag dhcpserver
                                  espcoredump dns_relay cli
                                  app_update esp_https_ota
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

# Capture the IDF panic-handler output into the RTC pre-crash ring.
# Wrap BOTH panic_print_str AND panic_print_char so we catch the calls
# from external objects (panic_handler.c / panic_arch.c / eh_frame_parser.c
# use panic_print_str; panic_print_hex/dec use panic_print_char). The
# linker's --wrap only intercepts undefined references between objects,
# so the intra-object panic_print_str → panic_print_char calls inside
# panic.c stay unwrapped and don't double-count.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=panic_print_char")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=panic_print_str")
# panic_print_hex/dec call panic_print_char intra-object inside panic.c,
# which --wrap cannot intercept. Wrap them too so the Backtrace's hex
# addresses and the register-dump decimals actually appear in the ring.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=panic_print_hex")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=panic_print_dec")
