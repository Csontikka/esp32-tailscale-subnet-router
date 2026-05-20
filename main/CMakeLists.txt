idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                            "tailscale_manager.c"
                            "tailscale_mtu.c"
                            "telemetry.c"
                            "lwip_route_hook.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer
                                  lwip microlink
                                  esp_http_client esp-tls mbedtls
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
