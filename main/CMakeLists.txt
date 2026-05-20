idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                            "tailscale_manager.c"
                            "tailscale_mtu.c"
                            "telemetry.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif esp_timer
                                  lwip microlink
                                  esp_http_client esp-tls mbedtls
                    INCLUDE_DIRS ".")
