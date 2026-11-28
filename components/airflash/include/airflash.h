#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the airflash companion inside your app (the airflash updater in `factory` does the flashing):
 *   TCP  <log_port, 3333>     log stream from the app's first line (idf.py airflash-monitor / airflash.py logs)
 *   GET  <http_port, 8080>/info               → {"role":"app", name, mac, project, version, state, auth, ...}
 *   GET  <http_port, 8080>/nonce              → a one-time challenge for the requests below
 *   POST <http_port, 8080>/reboot-to-updater  → hand over to the updater for the next flash
 *   GET/DELETE <http_port>/coredump          → the last crash dump (needs CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y)
 *
 * Pairing: if the updater install wrote a token into the `upd_cfg` partition, the log stream, /reboot-to-updater
 * and /coredump need proof of it (HMAC-SHA256 of a nonce; the token itself never crosses the network).
 * No token there = open mode: everything works without it, and a warning is logged at boot.
 *
 * Call it after your app has network (or turn on "Connect to Wi-Fi" in menuconfig → airflash). It doesn't block.
 * It always returns ESP_OK; problems are logged (e.g. port already in use, Wi-Fi without an SSID) and the app keeps
 * running, without airflash or the part that failed. No need for ESP_ERROR_CHECK().
 * It logs the reset reason (and, after a crash, where to get the crash dump).
 * On the first boot after a flash, the app is confirmed once airflash_start() has started and it has an IP address;
 * if that hasn't happened AIRFLASH_ROLLBACK_TIMEOUT_S after boot (failed, not called, no IP), the board rolls back
 * to the updater, so it stays flashable.
 * The log buffer survives software resets, crashes and watchdog resets, so the lines before a crash are kept,
 * including the panic handler's own text.
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
