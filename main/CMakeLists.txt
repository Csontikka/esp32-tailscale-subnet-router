idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                            "tailscale_manager.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif
                                  lwip microlink
                    INCLUDE_DIRS ".")
