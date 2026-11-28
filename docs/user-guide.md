# airflash user guide

airflash lets you flash and monitor an ESP32-S3 over Wi-Fi with the commands you already know. You plug in USB
**once** to install a small updater. After that, `idf.py airflash-flash` replaces your app over Wi-Fi, and
`idf.py airflash-monitor` shows its logs with crash addresses decoded to `file:line`, the same way `idf.py monitor`
does over USB.

- [Requirements](#requirements)
- [What airflash is](#what-airflash-is)
- [Flash layouts](#flash-layouts)
- [Install the updater (once per board, over USB)](#install-the-updater-once-per-board-over-usb)
- [Pairing](#pairing)
- [Add airflash to your app](#add-airflash-to-your-app)
- [Daily use](#daily-use)
- [Finding the board](#finding-the-board)
- [Logs](#logs)
- [When the app crashes or can't get online](#when-the-app-crashes-or-cant-get-online)
- [Crash dumps](#crash-dumps)
- [Board not found](#board-not-found)
- [Troubleshooting](#troubleshooting)
- [Settings reference](#settings-reference)
- [Security and known limitations](#security-and-known-limitations)
- [How it works](#how-it-works)

---

## Requirements

- **ESP32-S3.** Other chips aren't supported yet. Tested boards are listed in the [README](../README.md#tested-boards).
- **ESP-IDF 5.4.3.** Build the updater and your apps with the **same** ESP-IDF version. ESP-IDF's bootloader starts
  apps built with the same or a newer ESP-IDF, but not with an older one, and the bootloader is installed once with
  the updater over USB and never updated over Wi-Fi.
- **The Python that ESP-IDF installs.** The laptop tool uses only the standard library.
- **An `idf.py` project** (ESP-IDF's CMake build). Arduino IDE, PlatformIO with the Arduino framework and Rust with
  `esp-idf-sys` aren't supported.
- A 2.4 GHz Wi-Fi network that the board and your laptop share.

---

## What airflash is

airflash has three parts:

| Part | Where it runs | What it does |
|---|---|---|
| **Updater** | the board's `factory` partition, installed once over USB | joins Wi-Fi, receives your app and writes it to `ota_0`, checks it, boots it. Also serves the crash dump when your app can't |
| **airflash component** | inside your app | streams the app's logs, starting from its first lines. Lets the laptop ask the app to hand over to the updater. Confirms a freshly flashed app so it isn't rolled back. Serves crash dumps |
| **Laptop tool** | your laptop | the `idf.py airflash-*` commands, or `python airflash.py ...` |

A wireless flash goes like this: the tool asks the running app to reboot into the updater. The updater receives
the new app, writes and verifies it, then reboots into it, and the new app confirms itself.

---

## Flash layouts

The updater installs the partition layout once, over USB. Nothing changes it over Wi-Fi. There are three layouts,
and the updater build picks one from menuconfig → **Serial flasher config → Flash size**. Chips with 32 MB or more
use the 16 MB layout.

| Partition | Offset | 4 MB | 8 MB | 16 MB | Used for |
|---|---|---|---|---|---|
| bootloader | 0x0 | | | | 2nd-stage bootloader, comes with the updater install |
| partition table | 0x8000 | | | | this table |
| `nvs` | 0x9000 | 24 KB | 24 KB | 24 KB | your app's NVS. The updater never touches it |
| `otadata` | 0xF000 | 8 KB | 8 KB | 8 KB | records which app boots |
| `phy_init` | 0x11000 | 4 KB | 4 KB | 4 KB | radio calibration data |
| `upd_cfg` | 0x12000 | 16 KB | 16 KB | 16 KB | the updater's own settings, including the [pairing token](#pairing) |
| `factory` | 0x20000 | 1 MB | 1.5 MB | 1.5 MB | the **updater** |
| `ota_0` | | 2 MB | 5.3 MB | 8 MB | **your app**, replaced on every flash |
| `storage` | | 832 KB | 1 MB | 6.3 MB | data partition for your app's files (for example SPIFFS) |
| `coredump` | | 64 KB | 64 KB | 64 KB | your app's last crash dump |

Data partitions keep their contents when you flash a new app. `airflash-info` lists the board's partitions.

### Finding your flash size

- **The module marking**: on an ESP32-S3-WROOM-1-**N8R8**, N8 means 8 MB flash and R8 means 8 MB PSRAM.
- **Or ask the chip** over USB: `esptool.py -p <port> flash_id` prints `Detected flash size: 8MB`.

### Custom layouts

To change sizes, edit the updater's partition CSV for your flash size before you install it:
`updater/partitions_4mb.csv`, `partitions_8mb.csv` or `partitions_16mb.csv`. (`updater/partitions.csv` is generated
from the one that matches the flash size, so don't edit that one.) Keep these rules:

- Keep `nvs`, `otadata`, `phy_init`, `upd_cfg` and `factory` (the updater).
- Exactly one app slot for your app: `ota_0`.
- A `coredump` partition, so crashes can be read over Wi-Fi.
- Every data partition your apps use must exist under the same name and be at least as big as in the app's table.

A changed layout only reaches the board with a USB install (`idf.py -p <port> flash` in `updater/`). Data in
partitions that moved or changed size is lost.

### Your app's partition table is only used at build time

The board keeps the airflash layout whatever your app's own table says. ESP-IDF still needs a table to build the app
and check that it fits. On the board, though, your app looks up its data partitions **by name** in the airflash
layout. That means:

- Every data partition your app's table lists must exist on the board under the same name. If one is missing,
  `airflash-flash` refuses to flash:
  `error: app uses data partition 'storage2' (960 KB), the board has none`
- The board's partition should be at least as big as the one in your table. A smaller one, or one with a different
  subtype, only gets a warning: `warning: partition 'storage': app expects 2.0 MB, board has 1.0 MB`. Your app
  will then fail when it uses the missing space.
- The app itself can be as big as the board's `ota_0`: 2 MB, 5.3 MB or 8 MB.

Plan your app around `nvs`, `storage` and `coredump`.

---

## Install the updater (once per board, over USB)

You need the airflash repository and an ESP-IDF terminal (`. $IDF_PATH/export.sh`):

```bash
git clone https://github.com/karan90687/airflash.git
cd airflash/updater
```

**1. Set the Wi-Fi and the flash size.**

```bash
idf.py menuconfig
```

| Menu | Field | What to enter |
|---|---|---|
| airflash | Wi-Fi SSID (2.4 GHz) | for example `my-wifi`: the **same** network your app uses, and your laptop |
| airflash | Wi-Fi password | the network's password |
| airflash | mDNS hostname (`<name>.local`), empty = airflash-XXXX | leave it **empty**: the board becomes `airflash-XXXX`, where XXXX is the last 4 hex digits of its MAC, e.g. `airflash-9220` |
| airflash | Pairing token (empty = generate one) | leave it **empty** unless you want a token of your own: see [Pairing](#pairing) |
| Serial flasher config | Flash size | your board's flash size. It picks the [layout](#flash-layouts) |

- The ESP32-S3 only uses **2.4 GHz**. It can never join a network that is 5 GHz only. A dual-band router works
  as long as its 2.4 GHz band is on.
- **Be careful with the hostname field.** It comes right after the password field. Whatever you type there is
  announced on the network as the board's name, over mDNS and in the router's client list. If you type the
  password there by mistake, you publish it.
- menuconfig saves these settings in plain text in `updater/sdkconfig`. Don't commit that file.
- Forgot the Wi-Fi? The build warns when no SSID is set, and the updater says so at boot:
  [`no Wi-Fi set: ...`](#no-wi-fi-set-idfpy-menuconfig--airflash-then-flash-the-updater-again-over-usb).

**2. Flash it and watch it start.**

```bash
idf.py -p <port> flash monitor
```

`<port>` is for example `/dev/cu.usbmodem101` (macOS), `/dev/ttyACM0` (Linux) or `COM5` (Windows). If the board
held other firmware, you can run `idf.py -p <port> erase-flash` first for a clean start. That erases everything on
the chip, including any files the old firmware stored.

While building, it prints the board's [pairing token](#pairing):

```
airflash token: <32 hex characters>
  saved in ~/.airflash/token; the airflash tool on this laptop uses it automatically.
  Anyone else who should flash or watch this board needs this token (file or AIRFLASH_TOKEN).
```

When the updater is up, the monitor shows:

```
I (...) updater: updater 0.1.0 running because: no_app (reset: power_on)
I (...) updater: ready at http://10.42.0.104 (http://airflash-9220.local)
```

Quit the monitor with `Ctrl+]` and unplug USB. From now on any USB power supply or power bank is enough. If
`ready at` never appears, the board can't join Wi-Fi: see
[Updater never prints "ready at"](#updater-never-prints-ready-at).

### When your Wi-Fi network changes

There is no setup hotspot, by design. For a new network, reinstall the updater over USB with the new settings:

```bash
cd updater
idf.py menuconfig                  # airflash → new SSID / password
idf.py -p <port> flash monitor
```

A full `flash` also points the board back at the updater, so it's ready to receive an app. It writes this laptop's
token again. Your old app stays in `ota_0`, but it still has the old Wi-Fi settings. Give your app the new settings
too, then run `idf.py airflash-flash`.

### Updating the updater

```bash
cd updater
idf.py -p <port> app-flash
```

`app-flash` writes only the `factory` partition. The installed app, its rollback state, the pairing token and all
data partitions are kept.

### Never `idf.py flash` your app over USB

`idf.py flash` in your app's project writes your app's own bootloader and partition table over the airflash ones.
That removes the updater, and wireless flashing stops working. Over USB, `idf.py -p <port> monitor` is always safe.
If you flashed over USB by mistake, reinstall the updater (step 2 above) and then run `airflash-flash` for your app.

---

## Pairing

A board only lets a paired laptop flash it, reboot it into the updater, download its crash dump or read its log
stream. Both the updater and your app check this. `/info` stays open, so `airflash-find` can still list the board.

**How a board gets its token.** When you install the updater with `idf.py flash` in `updater/`, the build writes a
token into the board's `upd_cfg` partition. By default (menuconfig → airflash → **Pairing token (empty = generate
one)** left empty) it creates a random token once per laptop and saves it in `~/.airflash/token`, so every board you
install from the same laptop gets the same token.

The build takes the first token it finds:

1. menuconfig → airflash → Pairing token (`UPD_TOKEN`),
2. the `AIRFLASH_TOKEN` environment variable,
3. `~/.airflash/token`,
4. otherwise a new random one, saved in `~/.airflash/token`.

**Your own token.** Set one in menuconfig when a team shares one token, or to reinstall boards with a token you
already know. It can be 1–63 printable characters without spaces. A custom token is saved to `~/.airflash/token`
if that file doesn't exist yet. If the file exists with another token, it's left alone and the build says:

```
note: this laptop's airflash tool uses a different token (~/.airflash/token): set AIRFLASH_TOKEN to this one, or replace that file if all your boards use it
```

A short token gets `note: short token: someone on your Wi-Fi who records one login could guess it`. That's your
choice to make: the traffic isn't encrypted, so someone on your Wi-Fi can record one login and then try tokens
offline until one gives the same answer. A short token falls quickly to that, a random 32-character one doesn't.

The token stays only in the updater's local `sdkconfig` (git-ignored) and on the board. It's never compiled into
the firmware binary.

**How the laptop proves it.** The airflash tool reads `~/.airflash/token` automatically. The token itself never
crosses the network: the board sends a one-time challenge, and the laptop answers with an HMAC of it made with the
token. Someone listening can't learn the token directly or replay an answer. Uploads are signed together with the
image's SHA-256, so a relayed or altered upload is refused: `airflash: the upload doesn't match its signature`.

**Teammates and other laptops.** Anyone else who should flash or watch the board needs the same token. Copy
`~/.airflash/token` to the same place on their laptop, or have them set `AIRFLASH_TOKEN`. Treat the token like a
password. `--airflash-token <token>` (`--token` for `airflash.py`) overrides both for one command.

**What keeps or removes the token:**

| You do | Token on the board |
|---|---|
| `idf.py app-flash` in `updater/` | kept |
| `idf.py flash` in `updater/`, from the same laptop | written again: the same token, unless you changed the menuconfig token or `AIRFLASH_TOKEN` |
| `idf.py flash` in `updater/`, from another laptop | replaced by the token that build uses (its menuconfig token, `AIRFLASH_TOKEN` or `~/.airflash/token`) |
| `idf.py erase-flash`, or erasing the chip any other way | gone: reinstall the updater with `idf.py flash` |

`airflash-info` shows `pairing: on`, or `pairing: OFF (...)` with the reason. A board without a token warns on every
boot:
`no airflash token on this board: anyone on this network can flash it and read its logs (reinstall the updater over USB to set one)`.

Pairing is a **lock, not encryption**. See [Security and known limitations](#security-and-known-limitations).
The error messages are listed under [Pairing errors](#pairing-errors).

---

## Add airflash to your app

### 1. Add the component

From the ESP Component Registry:

```bash
idf.py add-dependency "karan90687/airflash^0.1.0"
```

Or in your project's `main/idf_component.yml` (create the file if it doesn't exist), the git repository:

```yaml
dependencies:
  karan90687/airflash:
    git: https://github.com/karan90687/airflash.git
    path: components/airflash
    version: v0.1.0
```

or a local clone:

```yaml
dependencies:
  airflash:
    path: /path/to/airflash/components/airflash
```

Registry and git dependencies are downloaded into `managed_components/karan90687__airflash/` on the next build. The
component brings in `espressif/mdns` itself. If `main/CMakeLists.txt` has a `REQUIRES` or `PRIV_REQUIRES` list, add
`airflash` to it.

### 2. Call `airflash_start()`

**A. Your app has its own networking** (`example_connect()`, its own Wi-Fi code, Ethernet). Call it once the board
has an IP:

```c
#include "airflash.h"

void app_main(void)
{
    // ... your NVS / netif / Wi-Fi setup, e.g. ESP_ERROR_CHECK(example_connect());
    airflash_start();
    // ... the rest of your app
}
```

**B. Your app has no networking.** Let airflash connect. In menuconfig → airflash, turn on
**Connect to Wi-Fi inside airflash_start()** (`CONFIG_AIRFLASH_WIFI_ENABLE`) and enter the same SSID and password as
the updater:

```c
#include "airflash.h"

void app_main(void)
{
    airflash_start();   // NVS + Wi-Fi + log stream + control port + confirm
    // ... your app
}
```

Leave the built-in Wi-Fi **off** if your app sets up networking itself. Otherwise airflash can't start: it would
create the default event loop a second time. With the built-in Wi-Fi on but no SSID set, it logs
`airflash: built-in Wi-Fi is on but no SSID is set: idf.py menuconfig → airflash`.

**`airflash_start()` never fails your app**, so don't wrap it in `ESP_ERROR_CHECK`. If something can't start (port
8080 or 3333 already used by your app, memory short), it logs an error and your app keeps running without airflash.
On the first boot after a wireless flash, an airflash that couldn't start means the board rolls back to the updater,
so it stays flashable, and `airflash-flash` says why:
[`the new app's airflash couldn't start ...`](#the-new-apps-airflash-couldnt-start--and-the-board-rolled-back).

Logs are captured from before `app_main()` wherever you call `airflash_start()`: a startup hook installs the
capture. The log stream and the control port start when you call it. On the USB monitor you will then see:

```
I (4504) airflash: airflash 0.1.0 in file_server 1 as airflash-9220.local: logs on tcp port 3333, control on http port 8080
I (4554) airflash: new app confirmed (valid)
```

If your app already runs mDNS with its own hostname, airflash keeps the app's name
(`airflash: keeping the app's mDNS name <name>.local`), and `airflash-find` and `airflash-info` show that name.
Otherwise the board is `airflash-XXXX`, as in the updater.

Your app doesn't need any pairing setting: it uses the token the updater install put on the board.

### 3. Turn on crash dumps, and pick a partition table

Use menuconfig → Core dump → Data destination → Flash, or put this in `sdkconfig.defaults`:

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

Keep the default ELF format. Without crash dumps, a crash only tells you
`the last boot crashed; to see where, enable CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`.

A good `sdkconfig.defaults` for a new app (set the flash size to your board's: `_4MB`, `_8MB` or `_16MB`):

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
# build-time only: the board keeps the airflash layout
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
```

With these settings ESP-IDF uses its "single app large + coredump" table: `nvs`, `phy_init`, a 1.5 MB app partition
and `coredump`. Every airflash layout has all of them. If your app is bigger than 1.5 MB or needs `storage`, use a
custom table with `CONFIG_PARTITION_TABLE_CUSTOM=y` and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`.
For an 8 MB board:

```
# Name,   Type, SubType,  Offset, Size
nvs,      data, nvs,      ,       0x6000
phy_init, data, phy,      ,       0x1000
factory,  app,  factory,  ,       0x550000
storage,  data, spiffs,   ,       0x100000
coredump, data, coredump, ,       64K
```

For other boards, use the board's sizes:

| Board flash | `factory` up to | `storage` up to |
|---|---|---|
| 4 MB | `0x200000` (2 MB) | `0xD0000` (832 KB) |
| 8 MB | `0x550000` (5.3 MB) | `0x100000` (1 MB) |
| 16 MB | `0x800000` (8 MB) | `0x650000` (6.3 MB) |

Don't pick "Two large size OTA partitions". It can't be combined with core dumps (see
[gen_esp32part.py: error](#gen_esp32partpy-error--is-a-directory)), and your app doesn't need OTA partitions: the
updater does the OTA.

### 4. Install the idf.py commands (symlink `idf_ext.py`)

ESP-IDF 5.x loads `idf.py` extensions only from the **project root** (a file named `idf_ext.py`). Symlink the
component's file there:

```bash
cd ~/my_project
# registry or git dependency (after the first build has downloaded the component):
ln -s managed_components/karan90687__airflash/tool/idf_ext.py idf_ext.py
# path dependency:
ln -s /path/to/airflash/components/airflash/tool/idf_ext.py idf_ext.py
```

Use a symlink, not a copy. A copy goes stale when the component updates, and newer commands then go missing
(`command "airflash-coredump" is not known to idf.py ...`). On Windows, copy the file and copy it again after every
component update.

### 5. Build once, then flash

```bash
idf.py build              # once: the airflash commands read the build output
idf.py airflash-flash
idf.py airflash-monitor
```

### Try it with the example first

`examples/basic` has no networking of its own, so it uses airflash's built-in Wi-Fi. Its `sdkconfig.defaults`
assumes 8 MB flash: on another board, set Serial flasher config → Flash size in menuconfig too.

```bash
cd examples/basic
idf.py menuconfig                                      # airflash → same SSID / password as the updater
ln -s ../../components/airflash/tool/idf_ext.py idf_ext.py
idf.py build
idf.py airflash-flash
idf.py airflash-monitor                                # "heartbeat N" every 5 s
```

---

## Daily use

Run these in your project's root folder, in an ESP-IDF terminal:

| idf.py | python airflash.py | What it does |
|---|---|---|
| `idf.py airflash-flash` | `flash <project>` | builds the app, finds the board, asks the running app to hand over to the updater, runs compatibility checks, uploads, then waits for the new app to confirm itself |
| `idf.py airflash-monitor` | `monitor <project>` | the real `idf.py monitor` over Wi-Fi: colors, addresses decoded to `file:line`. It keeps running through crashes and reboots. Quit with `Ctrl+]` |
| `idf.py airflash-logs` | `logs` | the plain log stream. The Python command doesn't need ESP-IDF |
| `idf.py airflash-coredump` | `coredump <project>` | downloads the last crash dump, clears it on the board and decodes it |
| `idf.py airflash-info` | `info` | shows what's running (app or updater), versions, rollback state, the last reset, pairing, whether a crash dump is waiting, and the board's partitions |
| `idf.py airflash-run` | `run` | only when the updater is running: boots the installed app again |
| `idf.py airflash-find` | `find` | lists the airflash boards on this network |

A normal flash looks like this:

```
app: file_server 1 (IDF v5.4.3, esp32s3), 853 KB
looking for the board at 10.42.0.104...
file_server 1 is running → asking it to reboot into the updater
waiting for the updater...
  uploading 100%  853 KB / 853 KB  (96 KB/s)
  verifying on the board (checksum, SHA-256, chip)...
waiting for the new app...
done: file_server 1 is running (valid)
```

### Options

| idf.py option | python option | Environment variable | What it does |
|---|---|---|---|
| `--airflash-host <ip or name>` | `--host` | `AIRFLASH_HOST` | use this board, without searching |
| `--airflash-token <token>` | `--token` | `AIRFLASH_TOKEN` | use this [pairing token](#pairing) instead of `~/.airflash/token` |
| `--airflash-subnet <net>` (only `airflash-find`) | `--subnet` (every command) | | scan this network instead of the laptop's /24, e.g. `10.0.0.0/22` |
| `--airflash-force` (only `airflash-flash`) | `--force` (`flash`) | | flash even if the compatibility checks fail |
| | `--no-build` (`flash`) | | upload the existing build as it is |
| | `--app-port`, `--log-port` | | only if you changed the component's ports (defaults 8080, 3333) |

idf.py options go after the action (`idf.py airflash-flash --airflash-host 10.42.0.104`). Python options go after
the command (`python airflash.py flash ~/my_project --host 10.42.0.104`).

### Using the Python tool directly

`airflash.py` lives next to `idf_ext.py`, in the component's `tool/` folder:

```bash
python /path/to/airflash/components/airflash/tool/airflash.py flash ~/my_project
python managed_components/karan90687__airflash/tool/airflash.py logs --host 10.42.0.104
```

`flash`, `monitor` and `coredump` run `idf.py` and need an ESP-IDF terminal. `logs`, `info`, `run` and `find` need
only Python 3.

### Checks before every upload

`airflash-flash` reads the app image and the board's `/info` before writing anything:

| Check | Result if it fails |
|---|---|
| chip: `app is built for esp32, the board is esp32s3` | error |
| size: `app is 6.1 MB, the board's app slot holds 5.3 MB` | error |
| chip revision: `app supports chip v0.0..v0.1, the board is v0.2` | error |
| data partitions: `app uses data partition 'storage2' (960 KB), the board has none` | error |
| partition table offset: `the app expects its partition table at 0x10000, the board's is at 0x8000: ...` ([fix](#the-app-expects-its-partition-table-at--the-boards-is-at-0x8000)) | error |
| `partition 'storage': app expects 2.0 MB, board has 1.0 MB` / `different type (...)` | warning |
| `this app doesn't include the airflash component: no wireless logs, and it never confirms itself, ...` | warning |
| `crash dumps are off, so a crash won't show where it happened: ...` | warning |

If there are errors, it prints `not flashing; fix the problems above or use --force`. If it had already asked the
app to hand over, it also prints `booted the previously installed app again`, so the board is never left without
its app.

An app **without** the component can still be flashed, but it never confirms itself. It runs until the next reset,
and then the board falls back to the updater. It also has no wireless logs and no hand-over, so flashing the next
app needs a reset (power-cycle the board) first.

---

## Finding the board

On a single home network there's nothing to configure. Each board has a unique name, `airflash-XXXX` (or your
app's own mDNS name, if your app runs mDNS itself), and both the updater and your app announce the mDNS service
`_airflash._tcp`.

The tool picks the board in this order:

1. `--airflash-host` / `--host`, or `AIRFLASH_HOST`.
2. The board remembered for this project in `build/airflash.json`, if it still answers there with the same MAC.
   Each project remembers its own board. `idf.py fullclean` forgets it.
3. One mDNS question, "who offers `_airflash._tcp`?". The board is matched by its MAC, so a new IP from DHCP doesn't
   matter: `board moved: 10.42.0.250 → 10.42.0.104`. With nothing remembered, the only board on the network is
   used: `using airflash-9220 at 10.42.0.104`.
4. A **full scan** of the laptop's /24, but only if mDNS gets no answer and no board is remembered, or when you pass
   `--subnet`:
   `scanning every address of 10.42.0.0/24 (a board in Wi-Fi power-save may stop answering for ~10 s)...`
5. If several boards answer and none is remembered, you get `several airflash boards: pick one with --host <ip>` and
   a table. `PAIRING` is `on`, or `OFF` for a board without a token, which anyone can flash:

```
IP           NAME           RUNNING  WHAT                    MAC                PAIRING
10.42.0.104  airflash-9220  app      file_server 1 (valid)   aa:bb:cc:dd:92:20  on
10.42.0.117  airflash-4c1e  updater  no app installed        aa:bb:cc:dd:4c:1e  OFF
```

**The tool never switches to a different board on its own.** If the remembered board is silent, for example because
it's rebooting or crash-looping, the tool doesn't scan. It asks mDNS in case the board moved, and otherwise waits for
it where it was last seen, even if other boards are visible:
`airflash-9220 isn't answering (rebooting, crash-looping or off?): waiting for it at 10.42.0.104`. Other boards it
sees are listed with the hint `to use another board: --airflash-host <ip> (or delete build/airflash.json)`.

**Why the scan is the last resort:** scanning a /24 makes the laptop send about 250 ARP broadcasts. An app in Wi-Fi
power save (the ESP-IDF default) can stop answering for about 10 s afterwards, even to ping, so the scan may miss
the very board it's looking for. The updater turns power save off and isn't affected.

### Bigger or stricter networks

- **Big networks** (a /16 office network): the default scan covers only the laptop's /24. Use
  `idf.py airflash-find --airflash-subnet 10.0.0.0/22`, or `--airflash-host`. If `find` sees exactly one board and
  the project has none remembered yet, it remembers it for the project.
- **Networks that block mDNS**: the tool falls back to the scan, which is unreliable for apps in power save. Use
  `--airflash-host`.
- **Guest networks with client isolation**: the laptop can't reach the board at all. Use another network.
- **macOS** may ask once whether your terminal app may "find and connect to devices on your local network". Allow
  it, because mDNS and the scan need it. If you denied it: System Settings → Privacy & Security → Local Network →
  turn on your terminal app (Terminal, iTerm2, VS Code ...).
- **The robust option:** reserve the board's IP in your router's DHCP settings and set it once:
  `export AIRFLASH_HOST=10.42.0.104`.

---

## Logs

`airflash-monitor` and `airflash-logs` show your app's log from its first lines, then follow it live. Every new
connection replays what's still in the board's buffer, so you see the boot even if you connect late. The log stream
needs the [pairing token](#pairing), like everything else except `/info`.

### What you see over Wi-Fi, and what only USB shows

| | Over Wi-Fi | Over USB |
|---|---|---|
| ROM bootloader (`rst:0x...`, `boot:0x...`) and 2nd-stage bootloader lines | **never** | yes |
| `cpu_start: Multicore app` and PSRAM detection lines, printed before airflash's first hook | no | yes |
| From `cpu_start: Pro cpu start user code` on: the app_init block (project, version, compile time, ELF SHA, ESP-IDF), efuse, `heap_init` and `spi_flash` lines, then `main_task` and your app | yes | yes |
| `ESP_LOGx`, `ESP_EARLY_LOGx`, `esp_rom_printf`, the task watchdog report | yes | yes |
| plain `printf()` / `puts()` | **no**: only `ESP_LOGx` goes through the log hook | yes |
| a reboot | `[airflash] ---- reboot ----`, then `boot #N, reset reason: ...` | the ROM/bootloader lines |
| lines longer than 191 characters | cut | whole |

The buffer **survives** software resets (`esp_restart()`), panics and watchdog resets, so the lines before a crash
are still there after the reboot. It **starts over** after a power-on, after deep sleep, and with every newly
flashed build. Pressing the **RESET (EN) button on the ESP32-S3** counts as a power-on reset, so it also starts the
buffer over.

### Notes in the stream

| Line | From | Meaning |
|---|---|---|
| `[airflash] ---- reboot ----` | board | the board rebooted (crash, watchdog, `esp_restart()`). The lines above it are from before the reboot |
| `[airflash: board unreachable at 10.42.0.104:3333, retrying...]` | tool | can't connect right now. It retries every second and continues without duplicates or gaps |
| `[airflash: the board's log buffer started over (power-off, new app or update)]` | tool | older lines are gone |
| `[airflash: N bytes of logs were overwritten while disconnected]` | tool | the buffer wrapped around while you were disconnected, or while your viewer fell behind: then the board closes the connection and the tool resumes from the oldest data still there |
| `[airflash: too many log viewers connected, try again later]` | board | all viewer slots are in use (default 3) |

### Crash text in the stream

When the app crashes, the original ESP-IDF panic text appears in the stream right where the crash happened: the
register dump, `Backtrace:` with its PC:SP pairs, and `|<-CORRUPTED` if the stack is damaged. ESP-IDF prints a few
lines from inside its panic handler straight to the UART, where airflash can't capture them:

- the headline `Guru Meditation Error: Core N panic'ed (...)`
- `ELF file SHA256: ...`
- `Rebooting...`

airflash re-prints these in the same format. `airflash-monitor` decodes the addresses to `file:line`, exactly as on
USB.

If the panic text couldn't be captured (for example with `CONFIG_AIRFLASH_PANIC_TEXT` off), the next boot prints a
shorter summary from the crash dump instead: `Guru Meditation Error (previous boot): task '...' panic'ed (...)`, plus
PC and backtrace.

### Log buffer size

`CONFIG_AIRFLASH_LOG_BUF_SIZE`, default 16384 bytes, range 2048–65536. The buffer lives in **internal RAM** and
costs that much permanently. It can't go in PSRAM: PSRAM's cache is thrown away on reset, which would lose exactly
the newest lines. Each log viewer uses one lwIP socket (`CONFIG_LWIP_MAX_SOCKETS`, shared with your app). The full
cost of the component is in the [README](../README.md#resource-cost).

---

## When the app crashes or can't get online

What happens depends on **when** it fails:

| When it fails | What the board does | What you see | What to do |
|---|---|---|---|
| A new app, on its first boot after a flash, before it confirms itself | rolls back to the updater and **stays there**, even across power cycles | `airflash-flash`: `the new app did not confirm itself and the board rolled back to the updater. ...` | `airflash-coredump`, fix, flash again. Or `airflash-run` to try the same app again |
| A confirmed app, after Wi-Fi is up | reboots and runs the app again | the crash text and `---- reboot ----` in the stream | `airflash-coredump` |
| A confirmed app, before Wi-Fi is up, on every boot | parks itself in the updater after 3 crashes in a row | `airflash-info`: `... running because the app crashed several times in a row right after boot ...` | `airflash-coredump`, fix, flash. Or `airflash-run` |
| A new app that isn't on the network within 60 s | aborts on purpose and rolls back | `the new app could not join Wi-Fi 'my-wifi' within 60 s and rolled back: ...` | fix the app's Wi-Fi settings and flash again |
| A new app whose airflash couldn't start (port in use, memory short) | rolls back, so the board stays flashable | `the new app's airflash couldn't start (...) and the board rolled back: ...` | [free the port or memory](#the-new-apps-airflash-couldnt-start--and-the-board-rolled-back) and flash again |
| A confirmed app that keeps running but never gets online | nothing: it doesn't crash, so neither guard catches it | the board isn't found | the [recovery pin](#last-resort-recovery-pin-or-usb), if you enabled it, or USB |

### A new app fails before it confirms itself

On its first boot after a flash, a new app is on probation until it confirms itself. `airflash_start()` does that by
default. If **anything** resets the board before then, the bootloader rolls back to the updater: a crash, a
watchdog, `abort()`, a failing `ESP_ERROR_CHECK`, even a power cut or a press of RESET. The board stays in the
updater, even across power cycles, until you flash again or run `airflash-run`. `run` boots the installed app again,
with the same first-boot protection.

The tool shows the app's last logs before the crash, followed by ESP-IDF's original panic text, both taken from the
crash dump:

```
airflash: the new app did not confirm itself and the board rolled back to the updater. It crashed.
last logs before the crash:
  I (8644) crash_app: first bme280 reading in 2 s
  I (9644) crash_app: first bme280 reading in 1 s
  Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.

  Core  0 register dump:
  PC      : 0x4200efab  PS      : 0x00060630  A0      : 0x8037d648  A1      : 0x3fcbf4e0
  ...
  Backtrace: 0x4200efa8:0x3fcbf4e0 0x4037d645:0x3fcbf500

  ELF file SHA256: 41bdae6e9
run `coredump` for the full decode
```

Without crash dumps it only says
``the new app did not confirm itself and the board rolled back to the updater. It crashed: run `coredump` to see where.``

In this case the updater serves the crash dump, and `idf.py airflash-coredump` works as usual. `airflash-info`
says `running because the new app crashed or failed before confirming itself, so the board rolled back`.

### A confirmed app crashes after Wi-Fi is up

A confirmed app is never rolled back. It reboots and runs again. The crash text shows up in the stream, and
`airflash-coredump` gets the dump. If it keeps crashing, the board is reachable for only a few seconds between
crashes. The tool keeps asking during that time (`flash` waits up to 40 s, `coredump` 30 s), so just run the
command and leave it.

### A confirmed app crashes before Wi-Fi is up, every boot (crash-loop guard)

Without help, such a board would never get on the network again. After **3 crashes in a row, each within 30 s of
boot**, the component marks the app invalid and the board parks itself in the updater:

- `airflash-info` explains it: ``running because the app crashed several times in a row right after boot, so the
  board parked itself in the updater: run `coredump` to see where, then fix and flash (or `run` to try it again)``.
- The crash dump is available through `airflash-coredump`.
- `airflash-flash` of a fixed app prints
  `note: the installed app kept crashing right after boot, so the board parked itself in the updater; flashing the new one`.

| Option | Default | |
|---|---|---|
| `CONFIG_AIRFLASH_CRASH_LOOP_COUNT` | 3 | crashes in a row before parking. `0` turns the guard off |
| `CONFIG_AIRFLASH_CRASH_LOOP_WINDOW_S` | 30 | a crash counts only if it happens within this many seconds of boot |

Brownout, power-on and software resets never count, and neither does the first boot after a flash (the rollback
handles that one).

**Caveat:** some apps reboot on purpose to retry, for example an `ESP_ERROR_CHECK` on a sensor that isn't ready yet,
or `example_connect()` giving up after its default 6 retries. Such an app may get parked. For a product, raise the
count or turn the guard off.

### A new app never gets on the network

If a new app isn't on the network within `CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S` (default 60 s) of its first boot, it
aborts with a message and rolls back. Typical causes are a wrong SSID or password, the router being off, or
`airflash_start()` never being called because the app's own Wi-Fi code retries forever. On USB and in the crash
dump, the abort reads like this:
`airflash: not on the network 60 s after boot (Wi-Fi 'my-wifi': reason 201 NO_AP_FOUND)`.
`airflash-flash` then explains it:

```
airflash: the new app could not join Wi-Fi 'my-wifi' within 60 s and rolled back:
  network not found (reason 201): wrong SSID, router off, or 5 GHz-only?
fix the app's Wi-Fi settings and flash again
```

**Slow first boots.** The 60 s count from power-up, so everything your app does before its network is up and
`airflash_start()` has run counts against them. A long first-boot job can trip the deadline even though nothing is
wrong. The usual one is formatting a big filesystem: ESP-IDF's file-server example formats the board's `storage`
partition on its first boot, which took about 30 s for the 6.3 MB partition of the 16 MB layout. If your app does
something like that before going online, raise `CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S` (menuconfig → airflash), or
start the network first.

| Wi-Fi reason | The tool says | Likely cause |
|---|---|---|
| 201 | network not found | wrong SSID, router off, or the network is 5 GHz only |
| 2, 15, 202, 204 | authentication failed | wrong password |
| 210, 211 | security mismatch | the network's security mode isn't accepted (WPA3-only?) |
| 212 | network out of range | signal too weak |
| 200 | connection lost | lost the router's signal |

Other messages in the same place:

- `joined Wi-Fi but was not on the network within 60 s ... the router gave it no IP address (DHCP)`: check the
  router's DHCP (address pool full?), then `airflash-run` to try the same app again.
- `never called airflash_start() within 60 s ... its own network code may be stuck, e.g. wrong Wi-Fi settings`,
  followed by the last logs.

### Confirming the app yourself, later

By default `airflash_start()` confirms a new app as soon as it's on the network, which proves that Wi-Fi and
airflash work. If your app can still fail later in its startup (camera, audio, sensors ...), confirm it yourself
once it's healthy:

```
CONFIG_AIRFLASH_AUTO_MARK_VALID=n
```

```c
#include "esp_ota_ops.h"

airflash_start();   // logs "new app pending verify: call esp_ota_mark_app_valid_cancel_rollback() once healthy"
init_camera();
init_audio();
esp_ota_mark_app_valid_cancel_rollback();   // now this build counts as good
```

Until that call, any reset sends the board back to the updater. This protects only the **first boot after a flash**:
once confirmed, the app stays confirmed.

### Last resort: recovery pin or USB

The recovery pin is a way back into the updater without a computer: hold a pin low for about 5 s while the board
resets, and the bootloader erases `otadata`, so the updater runs. It's useful when an app runs but never comes
online, so the tool can't ask it to hand over, and nothing crashes for the guards to catch.

It's **off by default**, because which pins are free differs from board to board. To turn it on, pick a GPIO your
board leaves unconnected (on the Seeed XIAO ESP32-S3, GPIO1 is pin D0), then in `updater/` run `idf.py menuconfig` →
Bootloader config → **GPIO triggers factory reset** and set the options below. `updater/sdkconfig.defaults` has the
same lines commented out: you can use them instead, but `sdkconfig.defaults` is only read when `updater/sdkconfig`
doesn't exist yet, so delete that file afterwards (and set the Wi-Fi again in menuconfig).

```
CONFIG_BOOTLOADER_FACTORY_RESET=y
CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET=1
CONFIG_BOOTLOADER_FACTORY_RESET_PIN_LOW=y
CONFIG_BOOTLOADER_HOLD_TIME_GPIO=5
# boot the updater after the reset
CONFIG_BOOTLOADER_OTA_DATA_ERASE=y
# erase no data partitions (ESP-IDF's default would erase your app's "nvs")
CONFIG_BOOTLOADER_DATA_FACTORY_RESET=""
```

The last two matter: without `OTA_DATA_ERASE` the updater doesn't start, and ESP-IDF's default for the partitions
to clear is `nvs`. Then reinstall over USB with `idf.py -p <port> flash`: the bootloader can only be changed over
USB, and `app-flash` doesn't write it.

To use it:

1. Connect the pin to GND.
2. Press and release RESET, or power the board on.
3. Keep the pin on GND for 5 s, then remove the wire.

`airflash-info` then says
``running because it was asked to (a `flash` hand-over, the recovery pin or a USB erase)``. Now run
`airflash-flash`, or `airflash-run` to boot the installed app again.

Without the pin, or if even that doesn't help, plug in USB: `idf.py -p <port> monitor` shows what the board is
doing, and reinstalling the updater always works.

---

## Crash dumps

- They are **standard ESP-IDF ELF core dumps, unmodified**. ESP-IDF writes them to the `coredump` partition when the
  app crashes. airflash only copies the bytes to your laptop, where ESP-IDF's own `idf.py coredump-info` decodes
  them: the crashing task, registers, backtraces of all tasks.
- They need `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` in your app. The board layout already has the partition.
- The board keeps **one** dump, like a mailbox. `airflash-coredump` saves it to `build/airflash_coredump.bin`, clears
  it on the board, and decodes it. The next crash overwrites it. If a dump is waiting, `airflash-info` says
  `` crash dump waiting (... KB): run `coredump` ``.
- The next download overwrites `build/airflash_coredump.bin`. To keep a dump, copy the file. To decode it again
  later: `idf.py coredump-info -c build/airflash_coredump.bin`.
- The last ~4 KB of logs before the crash travel inside the dump (`CONFIG_AIRFLASH_COREDUMP_LOG_TAIL`).
  `airflash-coredump` prints them first, as `last logs before the crash (from the dump):`, and `airflash-flash` shows
  them after a rollback: up to 25 of the app's lines before the crash, then ESP-IDF's original panic text (the Guru
  Meditation block). This helps when the crash happened before the log stream could be reached, e.g. after a
  rollback.

**`the dump is from another build`**: a dump holds addresses only, and only the exact ELF that crashed can turn them
into `file:line`. If you rebuilt in the meantime, you get:

```
airflash: the dump is from another build (ELF SHA 1a2b3c4d..., this build 5e6f7a8b...). Decode it with that build: idf.py coredump-info -c .../build/airflash_coredump.bin
```

The file is already saved at that point. Decode it in a checkout of the build that crashed. A rebuild of the same
source is usually not byte-identical, so keep the `.elf` of builds you leave running.

### Reading a stack overflow

```
***ERROR*** A stack overflow in task sensor_task has been detected.
```

The backtrace that follows points at FreeRTOS's task switch, which is where the overflow was noticed, not at the
function that overflowed. **The task name is the clue.** Raise that task's stack size: the stack depth argument of
`xTaskCreate()`, in bytes on ESP-IDF. For `main` it's `CONFIG_ESP_MAIN_TASK_STACK_SIZE`, and for `sys_evt` it's
`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`. In the task table of `airflash-coredump`'s output, that task's stack
shows as used up.

---

## Board not found

**`this laptop is on 10.0.1.x, but airflash-9220 was last seen on 10.42.0.x: is the laptop on the right Wi-Fi?`**
- Cause: the laptop is on a different network than the board. The tool can't reach a board on another network.
- Fix: connect the laptop to the board's Wi-Fi.

**`airflash-9220 is not answering on 10.42.0.0/24. Check: ...`**

```
airflash: airflash-9220 is not answering on 10.42.0.0/24. Check:
  - is the board powered?
  - is the router on, with 2.4 GHz enabled? (the ESP can't use 5 GHz)
  - did the Wi-Fi name or password change? then reinstall the updater over USB with the new settings
to see exactly why: plug in USB and run  idf.py -p <port> monitor
```

- Cause: the board is off, or it can't join the Wi-Fi.
- Fix: go through the list. On USB, the monitor shows why the board can't join. The updater prints the Wi-Fi reason
  with a hint there:

  ```
  W (3850) updater: Wi-Fi 'my-wifi': can't connect (reason 203)
  W (6260) updater: Wi-Fi 'my-wifi': can't connect (reason 205)
  I (7370) updater: ready at http://10.42.0.104 (http://airflash-9220.local)
  ```

  A few refusals like these right after a reset are normal: routers with protected management frames turn the board
  away for a few seconds while they check its old connection. A real problem keeps going, and after 30 s the updater
  adds `still not connected after 30 s: if the Wi-Fi name or password changed, reinstall the updater over USB with
  the new settings`. A changed network means
  [reinstalling the updater](#when-your-wi-fi-network-changes).

**`airflash-9220 isn't answering (rebooting, crash-looping or off?): waiting for it at 10.42.0.104`**
- Cause: the remembered board is silent right now. The tool never switches to another board on its own, so it waits
  even when other boards answer. It lists those with `to use another board: --airflash-host <ip> (or delete build/airflash.json)`.
- Fix: nothing, if this is the board you want. The tool continues when the board answers. If it never does, see the
  two entries above. To use one of the other boards, pass `--airflash-host <ip>` or delete `build/airflash.json`.

**`no airflash board found on 10.42.0.0/24 (powered? on the same Wi-Fi as this laptop? ...)`**
- Cause: nothing answered mDNS or the scan. The board is off, on another network, or on a bigger network than the
  laptop's /24. Or macOS hasn't given your terminal Local Network access.
- Fix: see [Bigger or stricter networks](#bigger-or-stricter-networks). `--airflash-host <ip>` always works if the
  laptop can reach the IP.

**`several airflash boards: pick one with --host <ip>`**
- Cause: more than one board answered, and none is remembered for this project.
- Fix: pick one from the table with `--airflash-host`. It's remembered from then on.

---

## Troubleshooting

Each entry gives the symptom (what you see), the cause and the fix. Search for your message.

### Install and build

#### Updater never prints "ready at"
- **Symptom:** the USB monitor shows `updater ... running because: ...`, but never `ready at http://...`.
- **Cause:** it can't join the Wi-Fi: wrong SSID or password, a 5 GHz-only network, or the router is off. The
  monitor prints the Wi-Fi reason with a hint.
- **Fix:** correct the settings in menuconfig → airflash and run `idf.py -p <port> flash monitor` again. Make sure the
  router's 2.4 GHz band is on.

#### `no Wi-Fi set: idf.py menuconfig → airflash, then flash the updater again over USB`
- **Symptom:** the updater logs this at boot. The build already warned that no Wi-Fi is set.
- **Cause:** the updater was built with an empty SSID, so it can't join any network.
- **Fix:** set the SSID and password in `idf.py menuconfig` → airflash in `updater/`, then
  `idf.py -p <port> flash monitor` again.

#### `invalid header: 0xffffffff`, over and over
- **Symptom:** after erasing the chip, the USB monitor repeats `invalid header: 0xffffffff` in a boot loop.
- **Cause:** the flash is empty, so there's no bootloader or app to start.
- **Fix:** reinstall the updater over USB: `idf.py -p <port> flash monitor` in `updater/`.

#### `ERROR: tool riscv32-esp-elf has no installed versions`
- **Symptom:** shown when you build or run `export.sh`, even for an ESP32-S3.
- **Cause:** the ESP32-S3's ULP coprocessor is RISC-V, so ESP-IDF needs the RISC-V compiler for it too. The
  ESP-IDF tools installation is incomplete.
- **Fix:** `cd $IDF_PATH && ./install.sh esp32s3`, then `. ./export.sh` again.

#### `gen_esp32part.py: error: ... Is a directory`
- **Symptom:** the build fails with
  `gen_esp32part.py: error: argument input: can't open '.../components/partition_table': [Errno 21] Is a directory`.
- **Cause:** the partition table is "Two large size OTA partitions" (`CONFIG_PARTITION_TABLE_TWO_OTA_LARGE`) and core
  dumps go to flash. ESP-IDF has no two-OTA-large table with a coredump partition, so the table file name ends up
  empty.
- **Fix:** use "Single factory app (large), no OTA" (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`) or a custom table
  with a `coredump` partition ([example](#3-turn-on-crash-dumps-and-pick-a-partition-table)). The app's table is
  only used at build time anyway.

#### `Error: app partition is too small for binary`
- **Symptom:** the build fails with `Error: app partition is too small for binary <app>.bin size 0x...`.
- **Cause:** your app is bigger than the app partition in *your* table, for example the 1.5 MB of "single app
  large". The board's slot is bigger: 2 MB, 5.3 MB or 8 MB, depending on its [layout](#flash-layouts).
- **Fix:** use a custom table with a bigger app partition, up to the board's `ota_0`
  ([example](#3-turn-on-crash-dumps-and-pick-a-partition-table)).

#### Settings in `sdkconfig.defaults` have no effect
- **Symptom:** you added options to `sdkconfig.defaults`, but the build ignores them. For example `airflash-flash`
  still warns `crash dumps are off, so a crash won't show where it happened`, or after a crash the stream says
  `the last boot crashed; to see where, enable CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`.
- **Cause:** `sdkconfig.defaults` is only read when `sdkconfig` doesn't exist yet. An existing project keeps its old
  values.
- **Fix:** set the options in `idf.py menuconfig`, or delete `sdkconfig` and rebuild. Deleting also drops anything
  you set only in menuconfig.

#### An option in `sdkconfig.defaults` is silently ignored
- **Symptom:** a single option keeps its default value although `sdkconfig` was freshly generated.
- **Cause:** an inline comment, as in `CONFIG_X=y  # comment`. That isn't a comment: the whole line is rejected
  without a message.
- **Fix:** put comments on their own line.

#### The project behaves like a different project
- **Symptom:** after copying `sdkconfig` from another project, you get a different partition table, a refusal
  (`app uses data partition ... the board has none`) or a build error.
- **Cause:** the copied `sdkconfig` wins over your `sdkconfig.defaults` and has drifted from it.
- **Fix:** don't copy `sdkconfig`. Delete it, keep `sdkconfig.defaults`, and rebuild.

#### `CONFIG_AIRFLASH_... undeclared` after updating the component
- **Symptom:** after updating airflash, the build fails with `'CONFIG_AIRFLASH_...' undeclared`.
- **Cause:** the new version added menuconfig options that your project's configuration hasn't picked up yet.
- **Fix:** run `idf.py reconfigure` once.

### idf.py commands

#### `command "airflash-flash" is not known to idf.py` / `No such option: --airflash-host`
- **Symptom:** `command "airflash-flash" is not known to idf.py and is not a Ninja target`, or
  `No such option: --airflash-host`.
- **Cause:** there's no `idf_ext.py` in the project root, so idf.py treats `airflash-flash` as a build target.
- **Fix:** [symlink `idf_ext.py`](#4-install-the-idfpy-commands-symlink-idf_extpy) into the project root.

#### `command "airflash-coredump" is not known to idf.py`, though other airflash commands work
- **Symptom:** older commands work, but a newer one is unknown.
- **Cause:** `idf_ext.py` is a stale copy from an older component version.
- **Fix:** replace the copy with a symlink. On Windows, copy the file again.

#### `airflash: build the project once first (idf.py build)`
- **Cause:** the commands find the airflash tool through the build output, and there's no build yet.
- **Fix:** `idf.py build`.

#### `airflash: the airflash component is not part of this project`
- **Cause:** the last build didn't include the component.
- **Fix:** check the dependency in `main/idf_component.yml` (and `REQUIRES`), then `idf.py build`.

#### `airflash: needs an ESP-IDF terminal (run export.sh first)`
- **Cause:** `flash`, `monitor` and `coredump` run `idf.py`, which isn't set up in this terminal.
- **Fix:** `. $IDF_PATH/export.sh`.

### Pairing errors

#### `this laptop has no airflash token yet: ...`
- **Symptom:** `this laptop has no airflash token yet: install the updater from this laptop (idf.py flash in updater/) or copy ~/.airflash/token from the laptop that did`.
- **Cause:** there's no `~/.airflash/token` on this laptop and no `AIRFLASH_TOKEN`.
- **Fix:** copy the token file from the laptop that installed the updater, or set `AIRFLASH_TOKEN`. Or reinstall
  the updater from this laptop over USB, which gives the board this laptop's token.

#### `... was installed from another laptop (or with another token): ...`
- **Symptom:** `airflash-9220 was installed from another laptop (or with another token): copy that laptop's ~/.airflash/token here, or set AIRFLASH_TOKEN`.
- **Cause:** this laptop has a token, but not the one the board was installed with.
- **Fix:** use that laptop's token: copy its `~/.airflash/token` here, set `AIRFLASH_TOKEN`, or pass
  `--airflash-token`.

#### `airflash: not paired: this board expects another airflash token`
- **Cause:** the board's own answer (HTTP 401) to a request with the wrong token or none, for example from `curl`
  or an old script.
- **Fix:** use the airflash tool with the right token, as in the two entries above.

#### `no airflash token on this board: ...`
- **Symptom:** the board prints at boot
  `no airflash token on this board: anyone on this network can flash it and read its logs (reinstall the updater over USB to set one)`,
  and `airflash-info` says `pairing: OFF (...)`.
- **Cause:** the `upd_cfg` partition is empty, for example because the chip was erased or the updater was written
  without `idf.py flash` (with `esptool.py`, or only `app-flash`).
- **Fix:** reinstall the updater with `idf.py -p <port> flash` in `updater/`.

### Flashing

#### `not flashing; fix the problems above or use --force`
- **Cause:** a [compatibility check](#checks-before-every-upload) failed. The `error:` lines above it say which one.
- **Fix:** fix your app, for example by renaming its data partition to one that exists on the board. Use
  `--airflash-force` only if you know the app will work anyway.

#### `the app expects its partition table at ..., the board's is at 0x8000`
- **Symptom:** `error: the app expects its partition table at 0x10000, the board's is at 0x8000: menuconfig → Partition Table → Offset of partition table → 0x8000`.
- **Cause:** your app was built with a different partition table offset (`CONFIG_PARTITION_TABLE_OFFSET`). It would
  look for its partitions in the wrong place on the board.
- **Fix:** set menuconfig → Partition Table → Offset of partition table to `0x8000`, then flash again.

#### `airflash: the upload doesn't match its signature`
- **Cause:** every upload is signed with the pairing token, together with the image's SHA-256. The board got an
  image that doesn't match its signature: it was altered or relayed on the way.
- **Fix:** flash again with the airflash tool. If it keeps happening, something on your network is tampering with
  the traffic.

#### `405 Method Not Allowed`
- **Symptom:** a `curl` or an old script uploading to `/update` gets `405`.
- **Cause:** the request reached the wrong role or port. App and updater share one IP: while your app runs, port 80
  is your app's own web server, and airflash's control port 8080 has no `/update`. The updater serves port 80 only
  while it's running.
- **Fix:** use `airflash-flash`. It asks the app to hand over first, then talks to the updater.
  `airflash-info` shows which one is running.

#### `/reboot-to-updater failed: 404 no updater installed (no factory partition)`
- **Cause:** the board no longer has the airflash layout, usually because the app was flashed over USB with
  `idf.py flash`.
- **Fix:** [reinstall the updater](#install-the-updater-once-per-board-over-usb), then `airflash-flash`.

#### `uploaded; the app has not answered yet (still joining Wi-Fi?)`
- **Cause:** the upload worked, but neither the new app nor the updater answered in time. The tool waits for the
  app's network deadline (`AIRFLASH_ROLLBACK_TIMEOUT_S`, 60 s by default) plus 40 s, because a new app that isn't
  online by then rolls back and the updater needs a few seconds to rejoin Wi-Fi. When the rollback shows up in time,
  the tool prints the reason itself.
- **Fix:** wait a minute, then run `airflash-info`. It shows either the app running, or the updater with the reason.

#### An old build has different Wi-Fi settings
- **Symptom:** `python airflash.py flash --no-build` uploads an older build, and the app never comes online.
- **Cause:** that build has other Wi-Fi settings and can't join. It rolls back with
  `the new app could not join Wi-Fi '...' within 60 s and rolled back: ...`.
- **Fix:** rebuild with the current settings (drop `--no-build`) and flash again.

#### `the updater refused the image: ...`
- **Symptom:** `image rejected (wrong chip or corrupted)`, `not an app image`, `app larger than ota_0` or
  `upload failed`.
- **Cause:** the updater checks the whole image before booting it, and the image failed. `upload failed` means the
  connection broke during the upload.
- **Fix:** rebuild and flash again. For `wrong chip`, check `idf.py set-target`.

### Network

#### Rejoining Wi-Fi after every crash takes several seconds
- **Symptom:** after each reboot the log shows `wifi:Association refused temporarily time ..., comeback time ... (TUs)`,
  and the board takes several seconds to come back.
- **Cause:** routers with protected management frames (PMF) make a station that vanished without saying goodbye
  wait before it can rejoin. A crash is exactly that.
- **Fix:** none needed, this is normal. It does make the reachable window shorter during crash loops, which is why
  the tool keeps polling.

#### The app answers slowly, or a scan misses it
- **Symptom:** `airflash-logs` lags, commands take longer while the app runs, or `find` misses the board while the
  updater is always found.
- **Cause:** your app's Wi-Fi is in power save, the ESP-IDF default. The updater turns power save off.
- **Fix:** usually fine as it is. For faster answers during development, call `esp_wifi_set_ps(WIFI_PS_NONE)` in your
  app. It draws more current.

#### `the new app's airflash couldn't start (...) and the board rolled back`
- **Symptom:** `airflash-flash` ends with
  `the new app's airflash couldn't start (<ESP_ERR_NAME>) and the board rolled back: is port 8080 or 3333 already used by the app, or is memory short? (menuconfig → airflash → ports) fix it and flash again`.
  On a confirmed app the same problem only shows as an `airflash:` error in the USB log, and the app keeps running
  without airflash.
- **Cause:** `airflash_start()` never fails your app, but it couldn't start its servers: your app already uses port
  8080, 3333 or the internal HTTP port 32800, or the heap was too short. Or the built-in Wi-Fi is on in an app that
  sets up networking itself (`airflash: wifi_start(...): event loop`). On the first boot after a wireless flash the
  board then rolls back to the updater, so it stays flashable.
- **Fix:** change `CONFIG_AIRFLASH_HTTP_PORT`, `CONFIG_AIRFLASH_LOG_PORT` or `CONFIG_AIRFLASH_HTTP_CTRL_PORT`
  (menuconfig → airflash), or free some heap, then flash again. With other ports, pass `--app-port` / `--log-port`
  to `airflash.py`: the idf.py commands assume 8080 and 3333. For the event loop error, turn
  `CONFIG_AIRFLASH_WIFI_ENABLE` off and call `airflash_start()` after your network is up.

#### `airflash: built-in Wi-Fi is on but no SSID is set: idf.py menuconfig → airflash`
- **Cause:** `CONFIG_AIRFLASH_WIFI_ENABLE` is on, but `CONFIG_AIRFLASH_WIFI_SSID` is empty.
- **Fix:** enter the same SSID and password as the updater in menuconfig → airflash, or turn the built-in Wi-Fi off
  if your app has its own networking.

### Logs

#### `printf()` lines are missing over Wi-Fi
- **Cause:** airflash captures the ESP-IDF log output (`ESP_LOGx`, `esp_rom_printf`). Plain `printf()` goes straight
  to USB.
- **Fix:** use `ESP_LOGI(TAG, ...)` for anything you want to see over Wi-Fi.

#### The first boot lines are missing
- **Symptom:** the stream starts at `cpu_start: Pro cpu start user code`. `rst:0x...`, the 2nd-stage bootloader
  lines, `cpu_start: Multicore app` and the PSRAM detection lines are missing. (The app_init block, efuse,
  `heap_init` and `spi_flash` lines that follow are there.)
- **Cause:** they're printed before airflash's first hook exists. ROM and bootloader output never reaches the app.
- **Fix:** none over Wi-Fi. Use the USB monitor when you need those lines.

#### The log history is gone after pressing RESET
- **Cause:** on the ESP32-S3, the RESET (EN) button is a power-on reset, and the buffer starts over after power-on.
  Software resets, panics and watchdog resets keep it.
- **Fix:** none needed. To reboot without losing logs, reboot from software (`esp_restart()`).

#### `idf.py -p socket://<ip>:3333 monitor` doesn't work
- **Cause:** the log stream needs the [pairing token](#pairing), which plain socket tools like `nc` or
  `idf.py -p socket://...` can't provide. A direct socket connection would also hang when the board reboots.
- **Fix:** use `idf.py airflash-monitor`. It goes through a local relay that pairs, reconnects and resumes without
  duplicates.

---

## Settings reference

### Updater (`updater/`, menuconfig)

| Option | Default | |
|---|---|---|
| airflash → `UPD_WIFI_SSID` / `UPD_WIFI_PASSWORD` | | the 2.4 GHz network, the same one as your app and your laptop |
| airflash → `UPD_HOSTNAME` | empty = `airflash-XXXX` | the board's mDNS and DHCP name. Never type a password here |
| airflash → `UPD_TOKEN` (Pairing token) | empty = generate one | the [pairing token](#pairing) to install: 1–63 printable characters without spaces. Empty: `AIRFLASH_TOKEN`, else this laptop's `~/.airflash/token` (created if missing) |
| Serial flasher config → Flash size (`CONFIG_ESPTOOLPY_FLASHSIZE_*`) | | your board's flash size: picks the [layout](#flash-layouts). 32 MB and more use the 16 MB layout |
| Bootloader config → GPIO triggers factory reset (`CONFIG_BOOTLOADER_FACTORY_RESET`) | off | the [recovery pin](#last-resort-recovery-pin-or-usb) |
| `AIRFLASH_TOKEN` environment variable, at build time | | the [pairing token](#pairing) to install when `UPD_TOKEN` is empty, instead of `~/.airflash/token` |

### Component (your app, menuconfig → airflash)

| Option | Default | |
|---|---|---|
| `AIRFLASH_WIFI_ENABLE` | off | `airflash_start()` brings up NVS and Wi-Fi itself and reconnects forever. Only for apps without networking |
| `AIRFLASH_WIFI_SSID` / `AIRFLASH_WIFI_PASSWORD` | | only with the above. Stored in plain text in `sdkconfig`, so don't commit it |
| `AIRFLASH_HOSTNAME` | empty = `airflash-XXXX` | same rule as the updater, so the board has one name in both. If your app already runs mDNS with its own hostname, airflash keeps the app's name |
| `AIRFLASH_HTTP_PORT` | 8080 | control port (`/info`, `/reboot-to-updater`, `/coredump`). Not 80: that's the updater's, and often your app's |
| `AIRFLASH_HTTP_CTRL_PORT` | 32800 | internal UDP port on 127.0.0.1 that every ESP-IDF HTTP server needs. Must differ from your app's own servers (apps with several servers typically use 32768, 32769 ...) |
| `AIRFLASH_LOG_PORT` | 3333 | TCP log stream |
| `AIRFLASH_LOG_CLIENTS` | 3 (1–5) | log viewers at the same time |
| `AIRFLASH_LOG_BUF_SIZE` | 16384 (2048–65536) | log buffer in internal RAM |
| `AIRFLASH_ROM_LOGS` | on | also capture `esp_rom_printf` output (early logs, the task watchdog report, core dump progress) |
| `AIRFLASH_EARLY_LOGS` | on | start capturing before `app_main()` |
| `AIRFLASH_PANIC_TEXT` | on | copy the panic handler's own text (Guru Meditation, registers, backtrace) into the stream |
| `AIRFLASH_COREDUMP_LOG_TAIL` | 4096 (0–16384) | bytes of the newest logs stored inside each crash dump. `0` = off. Costs that much internal RAM |
| `AIRFLASH_ROLLBACK_TIMEOUT_S` | 60 | a new app that isn't on the network by then rolls back. `0` = no limit |
| `AIRFLASH_AUTO_MARK_VALID` | on | `airflash_start()` confirms a new app once it's on the network |
| `AIRFLASH_CRASH_LOOP_COUNT` | 3 | crashes in a row before the board parks in the updater. `0` = off |
| `AIRFLASH_CRASH_LOOP_WINDOW_S` | 30 | a crash counts if it happens within this many seconds of boot |

### Laptop

| Setting | |
|---|---|
| `~/.airflash/token` | this laptop's [pairing token](#pairing), created by the first updater install |
| `AIRFLASH_TOKEN` | use this token instead of the file |
| `AIRFLASH_HOST` | use this board, without searching |
| `build/airflash.json` | the board remembered for this project. The tool never switches away from it on its own: delete the file (or `idf.py fullclean`) to use another board |

---

## Security and known limitations

- **Use airflash on networks you trust.** It's a development tool for your own Wi-Fi.
- **Pairing is a lock, not encryption.** Without the token nobody can flash the board or read its logs, and a
  recorded request can't be replayed. Uploads are signed together with the image's SHA-256, so a relayed or altered
  upload is refused. But the traffic itself isn't encrypted: someone who knows the Wi-Fi password and sniffs it can
  read the logs passing by. With a [short token](#pairing), a recorded login can also be guessed offline.
- **Secrets on disk and on the board.** The Wi-Fi password is compiled into the updater, and into apps that use
  airflash's built-in Wi-Fi. `sdkconfig` holds it in plain text: never commit `sdkconfig`, only
  `sdkconfig.defaults` without credentials. Treat `~/.airflash/token` like a password. Anyone who has the board in
  hand can read its flash over USB.
- **Not everything reaches Wi-Fi.** The stream starts at `cpu_start: Pro cpu start user code`. ROM and bootloader
  lines, and the `cpu_start: Multicore app` / PSRAM lines before it, are never visible over Wi-Fi, and plain
  `printf()` isn't captured: only `ESP_LOGx` and `esp_rom_printf`. Use USB when you need those.
- **A new Wi-Fi network means reinstalling the updater over USB.** There's no setup hotspot.
- **Discovery needs mDNS.** On networks that block mDNS, or are bigger than a /24, use `--airflash-host` or
  `--airflash-subnet` (see [Bigger or stricter networks](#bigger-or-stricter-networks)).
- **Wi-Fi power save** (the ESP-IDF default in apps) makes the app answer more slowly and can make a scan miss it.
  The updater turns power save off.
- **Only `idf.py` projects.** Arduino IDE, PlatformIO with the Arduino framework and Rust with `esp-idf-sys` aren't
  supported.
- **ESP32-S3 and ESP-IDF 5.4.3 only**, for now. See [Requirements](#requirements).

Known limitations:

- `idf.py -B <other build dir>` isn't supported by the airflash commands yet: they use `<project>/build`.
- A board whose `factory` partition holds your own app instead of the updater gives a confusing message on
  `/reboot-to-updater`. [Reinstall the updater](#install-the-updater-once-per-board-over-usb).
- airflash's log hook formats each `ESP_LOGx` line on the calling task's stack (~256 bytes extra). Give tight task
  stacks some headroom.

---

## How it works

**Boot chain.** The ROM bootloader in the chip loads the 2nd-stage bootloader from `0x0`. That bootloader was
installed with the updater, and OTA never changes it. It reads the partition table at `0x8000` and then `otadata`:

- `otadata` empty, or the app ABORTED or INVALID → it boots `factory`, the updater.
- `otadata` points at a good `ota_0` → it boots your app.

**Rollback states** (ESP-IDF's own app rollback):

```
updater writes the app → NEW → (bootloader, first boot) PENDING_VERIFY ─┬→ app confirms → VALID
                                                                         └→ reset before that → ABORTED → updater
```

`airflash_start()` makes the confirm call, unless `AIRFLASH_AUTO_MARK_VALID` is off. A USB flash never creates
PENDING_VERIFY: only an app installed by the updater is on probation. The crash-loop guard marks a confirmed app
INVALID, which also makes the bootloader start the updater.

**Hand-over.** `POST :8080/reboot-to-updater` makes your app erase `otadata` and restart, so the bootloader runs the
updater. The recovery pin, if enabled, does the same from the bootloader.

**Pairing.** The token lives in `upd_cfg`, written by the USB install. Every request except `/info`, and every log
stream connection, has to answer a one-time challenge from the board with an HMAC made with the token. An upload's
HMAC also covers the image's SHA-256, so the updater refuses an image that isn't the one that was signed.

**Ports.**

| Port | Who | What |
|---|---|---|
| 80 | updater | `POST /update`, `POST /run`, `GET /info`, `GET`/`DELETE /coredump` |
| 8080 | your app (component) | `GET /info`, `POST /reboot-to-updater`, `GET`/`DELETE /coredump` |
| 3333 | your app (component) | TCP log stream: replays the buffer, then live |
| 32800 (UDP, 127.0.0.1 only) | your app (component) | internal control socket of the HTTP server (`AIRFLASH_HTTP_CTRL_PORT`) |

App and updater share one IP (same MAC, same DHCP lease), so the tool asks both ports to find out which one is
running.

**Logs.** A startup hook (`esp_log_set_vprintf`, plus the ROM printf channel) copies every log line into a ring
buffer in RAM that startup never clears. That's why the buffer survives software resets and crashes, and why it
starts over on power-on.
