# airflash component

Flash and monitor your ESP32-S3 app over Wi-Fi with `idf.py`. This component runs inside your app. It:

- streams the app's logs over Wi-Fi from its first lines, kept across crashes and software reboots, including the
  original Guru Meditation text, so `idf.py airflash-monitor` decodes crashes to `file:line` as on USB;
- hands the board over to the airflash updater when the laptop wants to flash a new app;
- confirms a freshly flashed app once it's online, so it isn't rolled back, and rolls it back with the Wi-Fi
  reason if it never gets online;
- serves the last crash dump, with the app's last log lines inside it;
- parks a crash-looping app in the updater, so the board stays reachable.

It also ships the laptop tool (`tool/airflash.py`) and the `idf.py airflash-*` commands (`tool/idf_ext.py`).

## Requirements

- **The board needs the airflash updater**, installed once over USB. It does the actual flashing and sets the
  board's partition layout and pairing token: see [the airflash repository](https://github.com/karan90687/airflash).
- ESP32-S3, ESP-IDF 5.4.3 (the same version as the updater).
- Your app's own partition table is only used at build time: on the board, the app runs with the board's layout
  (which always has a `coredump` partition). Its data partitions (`nvs`, `storage`, ...) must exist on the board
  under the same names and be at least as big: see
  [Flash layouts](https://github.com/karan90687/airflash/blob/main/docs/user-guide.md#flash-layouts).

## Add it

```bash
idf.py add-dependency "karan90687/airflash^0.1.0"
```

It brings in `espressif/mdns`. If `main/CMakeLists.txt` has a `REQUIRES` or `PRIV_REQUIRES` list, add `airflash`.

Turn on crash dumps (menuconfig → Core dump → Data destination → Flash):

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

Install the `idf.py` commands with a symlink in your project root (ESP-IDF loads extensions only from there):

```bash
idf.py build
ln -s managed_components/karan90687__airflash/tool/idf_ext.py idf_ext.py
```

## API

```c
#include "airflash.h"

void app_main(void)
{
    // your network setup, e.g. ESP_ERROR_CHECK(example_connect());
    airflash_start();
    // ... the rest of your app
}
```

| Function | |
|---|---|
| `esp_err_t airflash_start(void)` | Starts the log stream (TCP 3333), the control port (HTTP 8080) and mDNS, and confirms a newly flashed app once it has an IP address. Call it after your app has network, or turn on the built-in Wi-Fi. Doesn't block, and never fails your app (see below) |
| `void airflash_capture_logs(void)` | Starts capturing logs now. Called automatically before `app_main()` unless `CONFIG_AIRFLASH_EARLY_LOGS` is off. Safe to call twice |

Don't wrap `airflash_start()` in `ESP_ERROR_CHECK`. If something can't start (port 8080 or 3333 already used by your
app, memory short), it logs an error and your app keeps running without airflash. On the first boot after a
wireless flash, an airflash that couldn't start makes the board roll back to the updater, so it stays flashable, and
`idf.py airflash-flash` says why.

The app needs the board's pairing token for flashing, rebooting into the updater, crash dumps and the log stream.
The token comes from the updater install; nothing to set in your app.

airflash formats each `ESP_LOGx` line on the calling task's stack (~256 bytes extra): give tight task stacks some
headroom.

## Options (menuconfig → airflash)

| Option | Default | |
|---|---|---|
| `CONFIG_AIRFLASH_WIFI_ENABLE` | off | `airflash_start()` brings up NVS and Wi-Fi itself and reconnects forever. Only for apps without networking of their own |
| `CONFIG_AIRFLASH_WIFI_SSID` / `CONFIG_AIRFLASH_WIFI_PASSWORD` | | with the above. Stored in plain text in `sdkconfig`: don't commit it |
| `CONFIG_AIRFLASH_HOSTNAME` | empty = `airflash-XXXX` | mDNS and DHCP name (XXXX = end of the MAC), the same rule as the updater. If your app already runs mDNS with its own hostname, airflash keeps that name (`airflash: keeping the app's mDNS name <name>.local`) |
| `CONFIG_AIRFLASH_HTTP_PORT` | 8080 | control port (`/info`, `/reboot-to-updater`, `/coredump`) |
| `CONFIG_AIRFLASH_HTTP_CTRL_PORT` | 32800 | internal UDP port on 127.0.0.1 that every ESP-IDF HTTP server needs. Must differ from your app's own servers (apps with several servers typically use 32768, 32769 ...) |
| `CONFIG_AIRFLASH_LOG_PORT` | 3333 | TCP log stream |
| `CONFIG_AIRFLASH_LOG_CLIENTS` | 3 (1–5) | log viewers at the same time, one lwIP socket each |
| `CONFIG_AIRFLASH_LOG_BUF_SIZE` | 16384 (2048–65536) | log buffer in internal RAM, kept across crashes and software resets |
| `CONFIG_AIRFLASH_ROM_LOGS` | on | also capture `esp_rom_printf` (early startup lines, task watchdog report) |
| `CONFIG_AIRFLASH_EARLY_LOGS` | on | start capturing before `app_main()` |
| `CONFIG_AIRFLASH_PANIC_TEXT` | on | copy the panic handler's own text (Guru Meditation, registers, backtrace) into the stream |
| `CONFIG_AIRFLASH_COREDUMP_LOG_TAIL` | 4096 (0–16384) | bytes of the newest logs stored inside each crash dump. `0` = off |
| `CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S` | 60 (0 = no limit) | a newly flashed app that isn't on the network by then rolls back to the updater |
| `CONFIG_AIRFLASH_AUTO_MARK_VALID` | on | confirm a new app as soon as it's online. Off: call `esp_ota_mark_app_valid_cancel_rollback()` yourself |
| `CONFIG_AIRFLASH_CRASH_LOOP_COUNT` | 3 (0 = off) | crashes in a row right after boot before the board parks in the updater |
| `CONFIG_AIRFLASH_CRASH_LOOP_WINDOW_S` | 30 | a crash counts if it happens within this many seconds of boot |

## Commands

| Command | What it does |
|---|---|
| `idf.py airflash-flash` | build, hand over to the updater, check, upload, wait for the new app |
| `idf.py airflash-monitor` | `idf.py monitor` over Wi-Fi, through crashes and reboots |
| `idf.py airflash-logs` | the plain log stream |
| `idf.py airflash-coredump` | download, clear and decode the last crash dump |
| `idf.py airflash-info` | what's running on the board |
| `idf.py airflash-run` | when the updater is running: boot the installed app again |
| `idf.py airflash-find` | list the airflash boards on this network |

Full documentation, including resource cost and security notes:
[airflash user guide](https://github.com/karan90687/airflash/blob/main/docs/user-guide.md).

MIT licensed.
