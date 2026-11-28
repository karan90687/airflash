cmake_minimum_required(VERSION 3.16)

# Shown in /info and in the boot log (esp_app_desc_t.version)
set(PROJECT_VER "0.1.0")

# ---- flash layout: partitions_<4mb|8mb|16mb>.csv, picked from menuconfig "Serial flasher config -> Flash size" ----
# Runs before ESP-IDF loads the config, so read the file it will use: sdkconfig, or the defaults on a first build.
# sdkconfig is a configure dependency, so changing the size in menuconfig re-runs this on the next build.
if(SDKCONFIG)
    get_filename_component(cfg_files "${SDKCONFIG}" ABSOLUTE)
else()
    set(cfg_files "${CMAKE_SOURCE_DIR}/sdkconfig")
endif()
if(NOT EXISTS "${cfg_files}")
    if(SDKCONFIG_DEFAULTS)
        set(cfg_files "${SDKCONFIG_DEFAULTS}")
    elseif(DEFINED ENV{SDKCONFIG_DEFAULTS})
        set(cfg_files "$ENV{SDKCONFIG_DEFAULTS}")
    else()
        set(cfg_files "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
    endif()
endif()
set(flash_size "")
foreach(f ${cfg_files})
    get_filename_component(f "${f}" ABSOLUTE)
    if(EXISTS "${f}")
        file(STRINGS "${f}" lines REGEX "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=y")
        foreach(l ${lines})  # the last one wins, as in Kconfig
            string(REGEX REPLACE "^CONFIG_ESPTOOLPY_FLASHSIZE_([0-9]+MB)=y.*" "\\1" flash_size "${l}")
        endforeach()
    endif()
endforeach()
if(flash_size STREQUAL "8MB")
    set(layout 8mb)
elseif(flash_size MATCHES "^(16|32|64|128)MB$")
    set(layout 16mb)  # bigger chips get the 16 MB layout; the rest of the flash stays unused
else()
    set(layout 4mb)
    if(NOT flash_size STREQUAL "4MB")
        # Not FATAL_ERROR: that would also block `idf.py menuconfig`, where this gets fixed. The 4 MB table
        # doesn't fit, so the build stops at the partition table with ESP-IDF's "Change the flash size" error.
        message(WARNING "airflash updater: flash size '${flash_size}' is too small, it needs 4 MB, 8 MB or 16 MB "
            "(or more). Set menuconfig -> Serial flasher config -> Flash size.")
    endif()
endif()
# ponytail: one generated partitions.csv per project dir; two build dirs with different sizes building at the
# same time would race. Each build re-copies its own on its next configure (the CSV is a configure dependency).
configure_file("${CMAKE_SOURCE_DIR}/partitions_${layout}.csv" "${CMAKE_SOURCE_DIR}/partitions.csv" COPYONLY)
message(STATUS "airflash updater: ${flash_size} flash -> partitions_${layout}.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(updater)

# Not FATAL_ERROR: that would also block `idf.py menuconfig`. STREQUAL, not if(NOT ...): an SSID "NO" or "0" is set.
if("${CONFIG_UPD_WIFI_SSID}" STREQUAL "")
    message(WARNING "airflash updater: no Wi-Fi set — idf.py menuconfig → airflash → Wi-Fi SSID/password, "
        "or the board can't join a network")
endif()

# ---- pairing token: build/upd_cfg.bin, written into "upd_cfg" by `idf.py flash` (not by app-flash) ----
# upd_cfg.py reads UPD_TOKEN from sdkconfig.json itself: the token stays out of build.ninja and the command line,
# and a ';' in it isn't split by CMake. The target always runs, so a new menuconfig token is picked up.
idf_build_get_property(python PYTHON)
idf_build_get_property(sdkconfig_json SDKCONFIG_JSON)
partition_table_get_partition_info(upd_cfg_size "--partition-name upd_cfg" "size")
add_custom_target(upd_cfg ALL
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/tools/upd_cfg.py" "${CMAKE_BINARY_DIR}/upd_cfg.bin" ${upd_cfg_size}
            "${sdkconfig_json}"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/upd_cfg.bin"
    VERBATIM)
add_dependencies(upd_cfg app)  # so the token is printed at the end of the build, where it is seen
add_dependencies(flash upd_cfg)
esptool_py_flash_to_partition(flash "upd_cfg" "${CMAKE_BINARY_DIR}/upd_cfg.bin")
