#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the airflash companion inside your app (the airflash updater in `factory` does the flashing):
 *   TCP  <log_port, 3333>     log stream from the app's first line (nc / idf.py monitor -p socket://ip:3333)
 *   GET  <http_port, 8080>/info               → {"role":"app", project, version, state, ...}
 *   POST <http_port, 8080>/reboot-to-updater  → hand over to the updater for the next flash
 *
 *   GET/DELETE <http_port>/coredump          → the last crash dump (needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y)
 *
 * Call it after your app has network (or turn on "Connect to Wi-Fi" in menuconfig → airflash).
 * It logs the reset reason (and a crash summary after a crash) and confirms a freshly installed app,
 * so the bootloader doesn't roll back to the updater.
 * The log buffer survives software resets, crashes and watchdog resets, so the lines before a crash are kept.
 */
esp_err_t airflash_start(void);

/**
 * Start capturing logs now. Called automatically before app_main() unless
 * "Capture logs from before app_main()" is off. Safe to call twice.
 */
void airflash_capture_logs(void);

#ifdef __cplusplus
}
#endif
