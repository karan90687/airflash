# 1. Partition table (partition-table.bin, written at 0x8000)
```
aa50   00     10       00000200    00003f00    6f74615f30...   00000000
magic  type   subtype  offset      size        label           flags
```
- numbers are stored in teh little endian format so, the bytes 00 00 02 00 mean 0x00020000
- 0x00 is teh app partition stores app.bin which esp executes 
- 0x10 OTA application slot 0 ota_0
- esp-idf has parition subtypes 0x00 app, 0x01 ota slot 0, 0x11 ota slot 1, 0x12 ota slot 2, 0x13 -0x1F additional ota slots 
- offset 0x00000200 where the partition starts 
- 00003f00 size of the ota_0 
- label 6f74615f30 this is ota_0 name
- flags there is no special flag

# 2. App image (esp32-peer.bin): what /update receives 

```
e9    06        02     3f         1c553740        ee   000000    0900  ...
magic segments  DIO    8MB|80MHz  entry address   WP   drive     chip ID 9 = ESP32-S3
```
- 0x0900 chip id is used by esptool.py to check the corect chip 
- 0x06 the firmware image has 6 segments each has load address(where to go in memory), data lenght and actual binary data. 
    - IRAM/DRAM - runtime data copied into internal ram by bootloader on boot 
    - IROM/DROM - code/constants remain in flash used via cache 
    - RTC_FAST/RTC_SLOW - memory used in low power mode, this memory also survives the crash `we will use this to store the logs of crash during run`
- 1c553740 entry address tells the bootloader where execution begins after the image loaded 
- 0x02 is the spi flash mode DIO dual i/o uses 2 data lines. it can also be QIO uses 4 data lines it is fast than DIO 
- 0xee WP is wire protect it is for spi and 0x0000(default) drive it is how strongly esp pins drive elecrtical signals. 
- Checksum + SHA-256 is checked by the esp_ota_end() it is present at the end of the esp32-peer.bin u'r app binary's end. 

# 3. Flashing ESP_IDF
- default table in esp when u dont have ota or custom partition table
```
for esp32s3 8mb flash
Partition	Flash offset	Size
NVS	        0x9000	      0x6000
OTA data	0xF000	      0x2000
PHY init	0x11000       0x1000
Factory app	0x20000       0x1E0000 (app.bin is here)
```
- First-stage ROM bootloader executes from internal ROM. (u can't edit this part)
- ROM bootloader loads the second-stage bootloader from SPI flash.
- Second-stage bootloader reads the partition table. (can be written by user via spi flash)
- It identifies the Factory application partition (or an OTA-selected partition).
- It loads/maps the application segments into their required memory regions. Application runs and calls app_main()
- NVS and PHY are not written by the flash they are handled by app itself. 

# stage 0 
- we have not made any factory partition in the partiotion table it only has (ota_0 and ota_1) so the first flash lands into the ota_0. 
```
ota_0: app v1 (with OTA code)  ← running
ota_1: empty → app v2 arrives here → reboot → ota_1 runs
                    next update  → goes into ota_0 → reboot → ota_0 runs ...
```
## imp finding
- when we have the blank ota data + a factory partition then the bootloader runs the factory... that's how the updater will become the default partition.

# stage 1 
- now we will be having a updater which will sit in teh factory partition. 
- updater will write the otadata which will tell the bootloader to laod ota_0 
```
updater (factory) receives app → writes ota_0 → checks it
   → esp_ota_set_boot_partition(ota_0)   → otadata: "boot ota_0"
   → reboot → bootloader reads otadata → loads ota_0
```
## when will bootloader fall back to factory (updater)
- otadata is blank, means a fresh install no app yet
- the app in ota_0 was aborted or crashed.
- the app itself calls `esp_ota_set_boot_partition(factory)` which erases otadata, then reboots 
## when updater gets a install new app 
- updater writes app --> ota_0, otadata: state = NEW --> reboot 
- bootloader: NEW --> PENDING_VERIFY --> runs the app
- the app must now call `esp_ota_mark_app_valid_cancel_rollback()`   → VALID
- if the app fails to call  `esp_ota_mark_app_valid_cancel_rollback()` or while uploading the new app the esp resets for any reason, then it will fallback again to factory parition and updater runs again ready for new app. 
- u have to call this `esp_ota_mark_app_valid_cancel_rollback()` once u'r wifi and other stuff are healthy in u'r app.. 
- 