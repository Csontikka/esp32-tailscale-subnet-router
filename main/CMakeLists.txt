idf_component_register(SRCS "main.c"
                            "log_capture.c"
                            "nvs_params.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event
                    INCLUDE_DIRS ".")
