# SD flight-recorder. Intercepts esp_log via vprintf and fwrites lines
# to rotating files on a FAT-mounted microSD (SDMMC 1-bit).
#
#   fatfs            - esp_vfs_fat_sdmmc_mount / esp_vfs_fat_info
#   sdmmc            - sdmmc_card_t, sdmmc_cmd.h (protocol layer)
#   esp_driver_sdmmc - SDMMC_HOST_DEFAULT / SDMMC_SLOT_CONFIG_DEFAULT (host)
#   esp_timer        - esp_timer_get_time (fsync cadence)
#   esp_app_format   - esp_app_get_description / esp_app_desc.h (boot marker)
#   microlink        - Phase B SNAP accessors (task states, DERP heartbeat)

idf_component_register(
    SRCS "sdlog.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc nvs_flash log vfs esp_timer
             esp_app_format microlink
)
