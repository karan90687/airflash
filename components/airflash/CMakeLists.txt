idf_component_register(SRCS "airflash.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update esp_partition spi_flash espcoredump
                                     esp_wifi esp_netif esp_event esp_timer nvs_flash lwip mbedtls)

# AIRFLASH_PANIC_TEXT: the panic handler prints straight to the UART/USB hardware, so its print functions are
# intercepted at link time. --wrap=X sends every call to X from another object file to __wrap_X (in airflash.c),
# which copies the text into the log buffer and calls the original, reachable as __real_X.
# Needs IDF 5.2+ (g_panic_abort_details). Skipped when the BLE controller's log option already wraps
# esp_panic_handler: two wrappers of one function can't link.
if(CONFIG_AIRFLASH_PANIC_TEXT AND NOT CONFIG_BT_LE_CONTROLLER_LOG_WRAP_PANIC_HANDLER_ENABLE
   AND "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.2")
    foreach(f esp_panic_handler panic_restart panic_print_char panic_print_str panic_print_hex panic_print_dec)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${f}")
    endforeach()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AIRFLASH_WRAP_PANIC=1)
endif()
