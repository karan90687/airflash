#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start wireless flashing + log streaming:
 *   POST /update      upload a new app .bin (OTA)
 *   GET  /logs?from=N read captured ESP_LOG output
 *
 * With "Connect to Wi-Fi" on (menuconfig → airflash) this also brings up
 * NVS + Wi-Fi. With it off, call this AFTER your project has network (IP).
 * Also confirms freshly OTA'd firmware for rollback (see README).
 */
esp_err_t airflash_start(void);

/**
 * Optional: start capturing logs early (first line of app_main) so boot logs
 * before airflash_start() also show up in /logs. Safe to call twice.
 */
void airflash_capture_logs(void);

#ifdef __cplusplus
}
#endif
