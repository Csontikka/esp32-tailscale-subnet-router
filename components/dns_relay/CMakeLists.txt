idf_component_register(SRCS "dns_relay.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lwip esp_netif nvs_flash esp_event log esp_timer)
