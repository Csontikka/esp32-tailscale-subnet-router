idf_component_register(SRCS "snmp_agent.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES freertos lwip nvs_flash log esp_app_format esp_timer
                                     esp_netif esp_driver_tsens)
