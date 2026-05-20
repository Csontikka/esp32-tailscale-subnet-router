idf_component_register(
    SRCS "net_diag.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip esp_timer log
)
