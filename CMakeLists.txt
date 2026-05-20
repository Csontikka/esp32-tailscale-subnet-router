# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PROJECT_VER lands in the esp_app_desc_t struct so the running firmware
# can report its version via the OTA APIs and the web UI's Status page.
set(PROJECT_VER "0.1.0-pre")

project(esp32_tsr)
