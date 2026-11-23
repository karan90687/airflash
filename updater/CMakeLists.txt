cmake_minimum_required(VERSION 3.16)

# Shown in /info and in the boot log (esp_app_desc_t.version)
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(updater)
