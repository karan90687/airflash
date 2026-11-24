idf_component_register(SRCS "airflash.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update esp_partition spi_flash espcoredump
                                     esp_wifi esp_netif esp_event nvs_flash lwip)
