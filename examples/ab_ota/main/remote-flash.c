#include "esp_log.h"
#include "remote_flash.h"

void app_main(void)
{
    ESP_ERROR_CHECK(remote_flash_start());  // Wi-Fi + /update + /logs + rollback confirm

    ESP_LOGI("app", "hello from OTA build v2 rolla h tere bhai ka mjak thori h ");
    // your application code goes here
}
