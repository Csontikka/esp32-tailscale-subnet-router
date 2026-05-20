# Web/admin password — salt + SHA-256 hash, persisted via raw NVS.
# Lives in its own component so both main (web_ui) and remote_console
# can PRIV_REQUIRES it without main+component cycles.

idf_component_register(
    SRCS "web_password.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash mbedtls esp_hw_support
)
