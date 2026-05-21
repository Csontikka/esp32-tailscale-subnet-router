idf_component_register(SRCS "cli.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES console esp_wifi nvs_flash
                                     esp_netif esp_system esp_event
                                     esp_timer esp_app_format
                                     web_password dns_relay)
