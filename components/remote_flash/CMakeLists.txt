idf_component_register(SRCS "remote_flash.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update esp_wifi esp_netif esp_event nvs_flash)
