# Remote syslog UDP forwarder. Intercepts esp_log via vprintf and posts
# RFC-3164 packets to a configurable server.

idf_component_register(
    SRCS "syslog_client.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip nvs_flash log esp_netif
)
