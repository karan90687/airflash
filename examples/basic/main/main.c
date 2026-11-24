#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "airflash.h"

static const char *TAG = "basic";

void app_main(void)
{
    ESP_LOGI(TAG, "hello from the basic example");   // already captured: airflash hooks logs before app_main

    ESP_ERROR_CHECK(airflash_start());  // Wi-Fi (menuconfig) + log stream + /info + /reboot-to-updater + confirm

    // your application code goes here; a heartbeat so the live log stream has something to show
    for (int i = 0;; i++) {
        ESP_LOGI(TAG, "heartbeat %d", i);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
