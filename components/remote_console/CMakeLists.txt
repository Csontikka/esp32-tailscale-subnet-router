# Remote console — TCP REPL on a configurable port. Auth-gated by the
# shared web_password hash. Wraps printf/puts/putchar/fputs/fwrite so
# command output from the active session is mirrored to the TCP client.

idf_component_register(
    SRCS "remote_console.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer nvs_flash console lwip esp_netif esp_event log
    PRIV_REQUIRES web_password
)

# Capture stdout from commands running inside the REPL session. Do NOT
# wrap vprintf — ESP_LOG goes through it internally and we still want
# log messages on the local serial console.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=printf"
    "-Wl,--wrap=puts"
    "-Wl,--wrap=putchar"
    "-Wl,--wrap=fputs"
    "-Wl,--wrap=fwrite"
)
