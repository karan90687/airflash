# airflash

Flash and monitor an ESP32-S3 **over Wi-Fi** with the `idf.py` commands you already know. You plug in USB once to
install a small updater on the board. After that, `idf.py airflash-flash` replaces your app over Wi-Fi, and
`idf.py airflash-monitor` is `idf.py monitor` over Wi-Fi, with crash addresses decoded to `file:line`.

```
 laptop                                          board (ESP32-S3)
 idf.py airflash-flash     ──┐                ┌─ updater (factory)  installed once over USB:
 idf.py airflash-monitor     ├──── Wi-Fi ─────┤                     receives, checks and boots your app
 idf.py airflash-coredump  ──┘                └─ your app (ota_0)   + airflash component:
                                                                    logs, hand-over, crash dumps
```

## Features

- **Wireless flash** with compatibility checks before anything is written (chip, revision, size, data partitions),
  and automatic rollback to the updater if the new app crashes or never gets online.
- **`idf.py monitor` over Wi-Fi**: colors and crash decoding, logs from the app's first lines, and the log buffer
  survives crashes and software reboots, so you still see what happened right before one.
- **The original Guru Meditation text** in the stream, as on USB.
- **Crash dumps over Wi-Fi**, standard ESP-IDF core dumps decoded on the laptop, with the app's last log lines inside.
- **Wi-Fi failure reasons in words** (wrong password, network not found ...) when a new app can't get online.
- **Crash-loop guard**: an app that crashes on every boot before it's online parks the board in the updater, so it
  stays reachable.
- **Board discovery** over mDNS, and each project remembers its board.
- **Pairing**: only laptops that have the board's token can flash it or read its logs.

## Requirements

- **ESP32-S3** (other chips aren't supported yet).
- **ESP-IDF 5.4.3.** Build the updater and your apps with the same version (see
  [why](docs/user-guide.md#requirements)).
- The Python that ESP-IDF installs. The laptop tool uses only the standard library.

## Quick start

### 1. Install the updater (once per board, over USB)

```bash
git clone https://github.com/karan90687/airflash.git
cd airflash/updater
idf.py menuconfig        # airflash → Wi-Fi SSID / password
                         # Serial flasher config → Flash size (4, 8 or 16 MB, see Flash layouts below)
idf.py -p <port> flash monitor
```

The build prints the board's token. It's created once per laptop and the tool uses it automatically (or set your
own: menuconfig → airflash → Pairing token, see [Pairing](docs/user-guide.md#pairing)):

```
airflash token: <32 hex characters>
  saved in ~/.airflash/token; the airflash tool on this laptop uses it automatically.
  Anyone else who should flash or watch this board needs this token (file or AIRFLASH_TOKEN).
```

Wait for `ready at http://<ip> (http://airflash-XXXX.local)`, quit the monitor with `Ctrl+]` and unplug USB. Any USB
power supply is enough from now on.

### 2. Add the component to your app

From the ESP Component Registry (once 0.1.0 is published there):

```bash
idf.py add-dependency "karan90687/airflash^0.1.0"
```

Or from git, in your project's `main/idf_component.yml`:

```yaml
dependencies:
  karan90687/airflash:
    git: https://github.com/karan90687/airflash.git
    path: components/airflash
    version: v0.1.0
```

### 3. Start it, and turn on crash dumps

```c
#include "airflash.h"

void app_main(void)
{
    // your network setup, e.g. ESP_ERROR_CHECK(example_connect());
    airflash_start();
    // ... the rest of your app
}
```

`airflash_start()` never fails your app, so don't wrap it in `ESP_ERROR_CHECK`. If something can't start (port 8080
or 3333 already used by your app, memory short), it logs an error and your app keeps running without airflash. On
the first boot after a wireless flash, an airflash that couldn't start makes the board roll back to the updater, so
it stays flashable, and `airflash-flash` tells you why.

No networking of your own? Turn on menuconfig → airflash → **Connect to Wi-Fi inside airflash_start()** and enter
the same network as the updater. Add to `sdkconfig.defaults` (or set in menuconfig):

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

Your app's own partition table is only used at build time: on the board the app runs with the board's layout, which
always has a `coredump` partition. Its data partitions (`nvs`, `storage`, ...) must exist on the board under the same
names: see [Add airflash to your app](docs/user-guide.md#add-airflash-to-your-app).

### 4. Install the idf.py commands

```bash
idf.py build                  # the first build downloads the component
ln -s managed_components/karan90687__airflash/tool/idf_ext.py idf_ext.py
```

ESP-IDF loads `idf.py` extensions only from the project root. Use a symlink, so it never goes stale (on Windows,
copy the file again after each component update).

### 5. Flash and watch

```bash
idf.py airflash-flash
idf.py airflash-monitor       # quit with Ctrl+]
```

## Commands

Run them in your project's root folder, in an ESP-IDF terminal.

| Command | What it does |
|---|---|
| `idf.py airflash-flash` | build, find the board, hand over to the updater, check, upload, wait for the new app to confirm itself |
| `idf.py airflash-monitor` | `idf.py monitor` over Wi-Fi, keeps running through crashes and reboots |
| `idf.py airflash-logs` | the plain log stream |
| `idf.py airflash-coredump` | download, clear and decode the last crash dump |
| `idf.py airflash-info` | what's running, versions, rollback state, last reset, pairing, partitions |
| `idf.py airflash-run` | when the updater is running: boot the installed app again |
| `idf.py airflash-find` | list the airflash boards on this network |

Options: `--airflash-host <ip>` (or `AIRFLASH_HOST`), `--airflash-token <token>` (or `AIRFLASH_TOKEN`),
`--airflash-force` (flash despite failed checks), `--airflash-subnet <net>` (`airflash-find`). The same commands work
without `idf.py` as `python airflash.py <command>`: see the [user guide](docs/user-guide.md#daily-use).

## Flash layouts

The updater installs one of three layouts, picked from menuconfig → Serial flasher config → Flash size when you build
it (32 MB and larger chips use the 16 MB layout). Every layout starts with `nvs` (24 KB), `otadata` (8 KB),
`phy_init` (4 KB) and `upd_cfg` (16 KB):

| Flash | `factory` (updater) | `ota_0` (your app) | `storage` | `coredump` |
|---|---|---|---|---|
| 4 MB | 1 MB | 2 MB | 832 KB | 64 KB |
| 8 MB | 1.5 MB | 5.3 MB | 1 MB | 64 KB |
| 16 MB | 1.5 MB | 8 MB | 6.3 MB | 64 KB |

Don't know your flash size? The module marking tells you (ESP32-S3-WROOM-1-**N8**R8: N8 = 8 MB flash, R8 = 8 MB
PSRAM), or run `esptool.py flash_id` and read `Detected flash size`. Custom layouts, and what your app's own
partition table has to match: [Flash layouts](docs/user-guide.md#flash-layouts).

## Resource cost

What the airflash component adds to your app, measured with ESP-IDF 5.4.3 on an ESP32-S3 app with default settings
(`idf.py size-components`):

| | Flash | Static internal RAM | Notes |
|---|---|---|---|
| airflash component | 10.2 KB | 22.5 KB + 0.7 KB IRAM | 16 KB log buffer, 4 KB crash-dump log tail, 2.3 KB other |
| mDNS (`espressif/mdns`) | 36.8 KB | 2.0 KB | pulled in by airflash |
| HTTP server (`esp_http_server` + `http_parser`) | 24.4 KB | 0 | only if your app doesn't use it already |
| core dump (`espcoredump`) | 9.7 KB | 0.9 KB | ESP-IDF's, once you enable core dumps |
| **Runtime heap** (measured on ESP32-S3 boards) | | **~15–17 KB** | task stacks, HTTP server, mDNS, sockets: internal heap measured just before and 3 s after `airflash_start()` (15.3 KB on the XIAO and the Korvo-2, 16.8 KB on the DevKit) |

- **Log buffer**: `CONFIG_AIRFLASH_LOG_BUF_SIZE`, 16 KB by default (2–64 KB), always in internal RAM.
- **Crash-dump log tail**: `CONFIG_AIRFLASH_COREDUMP_LOG_TAIL`, 4 KB by default, `0` turns it off.
- **Tasks**: log server (4 KB stack), HTTP server (ESP-IDF default, 4 KB), mDNS (ESP-IDF default, 4 KB), and a 4 KB
  task only on the first boot after a flash, until the app is confirmed.
- **Sockets** (shared with your app through `CONFIG_LWIP_MAX_SOCKETS`): 2 HTTP connections, up to 3 log viewers
  (`CONFIG_AIRFLASH_LOG_CLIENTS`), plus the listening sockets.
- **Ports**: 8080 (control, HTTP) and 3333 (log stream, TCP) on the app, plus UDP 32800 on 127.0.0.1, an internal
  port every ESP-IDF HTTP server needs (`CONFIG_AIRFLASH_HTTP_CTRL_PORT`). The updater uses port 80 while it runs.
- **CPU**: negligible.

## Security and limitations

- Use airflash on **networks you trust**. Pairing locks the board: without the token nobody can flash it or read
  its logs, and uploads are signed together with the image's SHA-256, so a replayed, relayed or altered upload is
  refused. It does **not encrypt**: someone who knows the Wi-Fi password and sniffs the traffic can read logs
  passing by.
- The Wi-Fi password is compiled into the updater (and into apps that use airflash's built-in Wi-Fi). Never commit
  `sdkconfig`.
- Over Wi-Fi the log starts at `cpu_start: Pro cpu start user code`: you never see ROM or bootloader lines, and
  plain `printf()` isn't captured, only `ESP_LOGx` and `esp_rom_printf`.
- A new Wi-Fi network means reinstalling the updater over USB.
- Networks that block mDNS, or are bigger than a /24, need `--airflash-host` or `--airflash-subnet`.
- Only `idf.py` (ESP-IDF CMake) projects. Not Arduino IDE, PlatformIO with the Arduino framework, or Rust with
  `esp-idf-sys`.
- The airflash commands use `<project>/build`: `idf.py -B <other build dir>` isn't supported yet.
- airflash formats each `ESP_LOGx` line on the calling task's stack (~256 bytes extra): give tight task stacks some
  headroom.

Details: [Security and known limitations](docs/user-guide.md#security-and-known-limitations).

## Tested boards

| Board | Flash | Status |
|---|---|---|
| Seeed XIAO ESP32-S3 | 8 MB | ✅ |
| ESP32-S3-Korvo-2 (16 MB flash, 8 MB PSRAM) | 16 MB | ✅ |
| ESP32-S3 DevKit (16 MB flash, 8 MB PSRAM, USB-to-UART port) | 16 MB | ✅ |

## Documentation

- [User guide](docs/user-guide.md): install, daily use, logs, crash handling, troubleshooting, settings.
- [Component README](components/airflash/README.md): the API and menuconfig options.

## License

MIT, see [LICENSE](LICENSE).
