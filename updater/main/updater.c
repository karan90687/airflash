/*
 * Factory updater: lives in the `factory` partition and is never overwritten by OTA.
 *
 * The bootloader only runs it when there is no user app to boot:
 *   - fresh install (otadata blank, ota_0 empty)
 *   - the app in ota_0 reset before confirming itself (rollback → otadata says ABORTED)
 *   - the app crashed right after boot several times in a row (the airflash component marked it INVALID)
 *   - recovery pin held at reset, or otadata erased (over USB, or later by the app hook)
 *
 * HTTP API (port 80):
 *   POST /update  body = app .bin  → write ota_0, verify, boot it
 *   POST /run                      → boot the app already in ota_0 (e.g. after using the recovery pin)
 *   GET  /info                     → JSON: name, mac, auth, updater, chip, flash, partitions, installed app, why we're running
 *   GET  /nonce                    → a one-time challenge for the protected requests
 *   GET / DELETE /coredump         → crash dump of an app that crashed before it could serve it itself
 *
 * Pairing: the install over USB writes a token into the `upd_cfg` partition. With one there, /update, /run and
 * /coredump need "X-Airflash-Auth: <nonce>:<HMAC-SHA256(token, nonce, method, path)>": the token never crosses the
 * network. POST /update also sends "X-Airflash-Body: <SHA-256 of the body>", signed with the rest, and the board
 * checks it against what arrived, so a signed request can't carry someone else's image. Lock only, nothing is
 * encrypted. No (valid) token = open mode, everything works without auth (a sent X-Airflash-Body is still checked).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_core_dump.h"
#include <sys/param.h>
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/constant_time.h"

static const char *TAG = "updater";
static char s_name[64];  // mDNS + DHCP hostname, "name" in /info (63 = longest mDNS label)

// Empty hostname setting → "airflash-XXXX" from the MAC's last 2 bytes, so boards on one network don't all
// claim the same .local name. The airflash component uses the exact same rule, so a board keeps its name in both.
static void board_name(void)
{
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);  // factory MAC from eFuse: works before Wi-Fi is started
    if (CONFIG_UPD_HOSTNAME[0]) strlcpy(s_name, CONFIG_UPD_HOSTNAME, sizeof s_name);
    else snprintf(s_name, sizeof s_name, "airflash-%02x%02x", m[4], m[5]);
}

/* ---------- pairing (same scheme as the airflash component) ---------- */

static char s_key[65];  // the token = the HMAC key; "" = open mode

static void hex(const uint8_t *b, size_t n, char *out)  // out: 2n + 1 chars, lowercase
{
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = "0123456789abcdef"[b[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[b[i] & 15];
    }
    out[2 * n] = '\0';
}

// upd_cfg at offset 0: "AFCFG001", token (64 bytes ASCII, NUL-padded), CRC32 of those 72 bytes (zlib's, little-endian).
// Missing, unreadable, blank or a bad CRC: s_key stays "" (open mode).
static void load_token(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "upd_cfg");
    uint8_t b[76];
    if (!p || esp_partition_read(p, 0, b, sizeof b) != ESP_OK || memcmp(b, "AFCFG001", 8) != 0 ||
        esp_rom_crc32_le(0, b, 72) != (b[72] | b[73] << 8 | b[74] << 16 | (uint32_t)b[75] << 24)) {
        return;
    }
    memcpy(s_key, b + 8, 64);  // s_key[64] stays '\0'
}

// mac_hex == lowercase hex HMAC-SHA256(token, msg)? Compared in constant time.
static bool hmac_ok(const char *msg, const char *mac_hex)
{
    uint8_t mac[32];
    char want[65];
    if (strlen(mac_hex) != 64 ||
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)s_key, strlen(s_key),
                        (const uint8_t *)msg, strlen(msg), mac) != 0) {
        return false;
    }
    hex(mac, sizeof mac, want);
    return mbedtls_ct_memcmp(want, mac_hex, 64) == 0;
}

// GET /nonce: a challenge for one protected request. Single use, valid 60 s, the newest NONCES are kept.
// Only the httpd task uses the pool: no lock.
#define NONCES 8
#define NOT_PAIRED "airflash: not paired: this board expects another airflash token"
static struct {
    char hex[33];  // "" = free or used
    int64_t at;
} s_nonces[NONCES];
static unsigned s_nonce_next;

static esp_err_t nonce_get(httpd_req_t *req)
{
    unsigned i = s_nonce_next++ % NONCES;  // replaces the oldest
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    hex(r, sizeof r, s_nonces[i].hex);
    s_nonces[i].at = esp_timer_get_time();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, s_nonces[i].hex);
}

static bool nonce_take(const char *n)
{
    for (int i = 0; i < NONCES; i++) {
        if (s_nonces[i].hex[0] && strcmp(s_nonces[i].hex, n) == 0) {
            s_nonces[i].hex[0] = '\0';  // used up, whether the rest of the request is right or not
            return esp_timer_get_time() - s_nonces[i].at < 60 * 1000000LL;
        }
    }
    return false;
}

// Protected requests carry "X-Airflash-Auth: <nonce>:<hmac>", hmac = HMAC-SHA256(token, "<nonce>\n<METHOD>\n<path>"),
// with "\n<body_sha>" appended when body_sha isn't NULL (POST /update).
static bool authorized(httpd_req_t *req, const char *body_sha)
{
    if (!s_key[0]) return true;  // open mode
    char h[100], msg[128];       // h: 32 + ':' + 64; msg: at most 32+1+6+1+9 or, for /update, 32+1+4+1+7+1+64
    if (httpd_req_get_hdr_value_str(req, "X-Airflash-Auth", h, sizeof h) != ESP_OK || strlen(h) != 97 || h[32] != ':') {
        return false;
    }
    h[32] = '\0';
    snprintf(msg, sizeof msg, "%s\n%s\n%.*s%s%s", h, http_method_str(req->method), (int)strcspn(req->uri, "?"), req->uri,
             body_sha ? "\n" : "", body_sha ? body_sha : "");
    return nonce_take(h) && hmac_ok(msg, h + 33);
}

/* ---------- helpers ---------- */

static const esp_partition_t *app_slot(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

// Why did the bootloader pick the updater instead of the user app?
static const char *why_running(void)
{
    const esp_partition_t *app = app_slot();
    esp_app_desc_t desc;
    if (!app || esp_ota_get_partition_description(app, &desc) != ESP_OK) {
        return "no_app";                        // nothing valid in ota_0 yet
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(app, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_ABORTED) return "app_rolled_back";  // app reset/crashed before confirming
        if (st == ESP_OTA_IMG_INVALID) return "app_crash_loop";   // airflash parked it: crashed on every boot
    }
    return "requested";                         // otadata cleared: recovery pin, USB erase, or app hook
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "external_pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "usb";
    default:               return "other";
    }
}

/* ---------- POST /update: stream the uploaded app into ota_0 ---------- */

// "X-Airflash-Body: <sha256 hex of the body>" is part of the signed message, and the body must hash to it: a signed
// /update seen on the network can't be relayed with another image. Required with a token, checked whenever sent.
// e.g. token 00112233445566778899aabbccddeeff, nonce 0123456789abcdef0123456789abcdef, body "hello":
//   X-Airflash-Body: 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
//   X-Airflash-Auth: 0123456789abcdef0123456789abcdef:e3d8eba5c8baafd90d87afe824183cdf01c66e5df91c9984d83a106f9f11e7bc
static esp_err_t update_post(httpd_req_t *req)
{
    char want[65], got[65];  // 64 hex + '\0'
    esp_err_t h = httpd_req_get_hdr_value_str(req, "X-Airflash-Body", want, sizeof want);
    bool has_sha = h != ESP_ERR_NOT_FOUND;
    if (has_sha && (h != ESP_OK || strspn(want, "0123456789abcdef") != 64)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "airflash: X-Airflash-Body must be the body's SHA-256, 64 lowercase hex");
    }
    if (s_key[0] && !has_sha) ESP_LOGW(TAG, "upload without X-Airflash-Body refused: the airflash tool is too old?");
    if ((s_key[0] && !has_sha) || !authorized(req, has_sha ? want : NULL)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);  // before the body
    }

    // From factory, the "next" OTA partition is ota_0. It can never return factory itself.
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota_0 partition");
    if (req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "app larger than ota_0");
    }
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)req->content_len, part->label);

    // SEQUENTIAL_WRITES: erase sector by sector while writing, instead of erasing 5 MB up front
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    char *buf = malloc(4096);
    int left = req->content_len, timeouts = 0;
    while (left > 0 && buf) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) break;
        mbedtls_sha256_update(&sha, (const uint8_t *)buf, r);
        // the first write also checks the image magic byte (0xE9)
        if ((err = esp_ota_write(ota, buf, r)) != ESP_OK) break;
        left -= r;
    }
    free(buf);
    uint8_t d[32];
    bool hashed = mbedtls_sha256_finish(&sha, d) == 0;
    mbedtls_sha256_free(&sha);
    if (left > 0) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "upload aborted, %d bytes missing (%s)", left, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   err == ESP_ERR_OTA_VALIDATE_FAILED ? "not an app image" : "upload failed");
    }
    hex(d, sizeof d, got);
    if (has_sha && (!hashed || strcmp(got, want) != 0)) {  // before esp_ota_end: ota_0 is never marked bootable
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "upload doesn't match its X-Airflash-Body: not installed");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "airflash: the upload doesn't match its signature");
    }

    // esp_ota_end verifies the whole image: chip ID, segments, checksum, SHA-256
    if ((err = esp_ota_end(ota)) != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image rejected (wrong chip or corrupted)");
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        ESP_LOGI(TAG, "installed %s %s (IDF %s)", desc.project_name, desc.version, desc.idf_ver);
    }

    // Writes a new otadata record (state NEW). The bootloader turns it into PENDING_VERIFY
    // on the next boot; the app must then confirm itself or it rolls back to us.
    if ((err = esp_ota_set_boot_partition(part)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    httpd_resp_sendstr(req, "OK, booting the new app\n");
    ESP_LOGI(TAG, "rebooting into %s", part->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---------- POST /run: boot the app that is already installed ---------- */

static esp_err_t run_post(httpd_req_t *req)
{
    if (!authorized(req, NULL)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);

    const esp_partition_t *app = app_slot();
    esp_app_desc_t desc;
    if (!app || esp_ota_get_partition_description(app, &desc) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no app installed");
    }
    esp_err_t err = esp_ota_set_boot_partition(app);  // also re-verifies the image
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

    httpd_resp_sendstr(req, "OK, booting the installed app\n");
    ESP_LOGI(TAG, "booting installed app %s %s", desc.project_name, desc.version);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---------- GET / DELETE /coredump: the crash dump of an app that never got to serve it itself ---------- */
// An app that crashes before confirming is rolled back to us; its core dump is still in the "coredump"
// partition (flash survives), and these core dump calls work even though the updater doesn't make dumps itself.

static size_t coredump_size(void)
{
    size_t addr, size;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK ? size : 0;
}

static esp_err_t coredump_get(httpd_req_t *req)
{
    if (!authorized(req, NULL)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump");
    httpd_resp_set_type(req, "application/octet-stream");
    static char buf[1024];
    for (size_t off = 0; off < size; off += sizeof buf) {
        size_t n = MIN(sizeof buf, size - off);
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK || httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t coredump_delete(httpd_req_t *req)
{
    if (!authorized(req, NULL)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return httpd_resp_sendstr(req, "OK, core dump erased\n");
}

/* ---------- GET /info: everything the laptop tool needs to decide if an app fits ---------- */

static const char *app_state_str(const esp_partition_t *app)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(app, &st) != ESP_OK) return "not_selected";  // no otadata record
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static esp_err_t info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", s_name);  // the laptop tool finds the board by name/mac after its IP changes
    cJSON_AddBoolToObject(root, "auth", s_key[0] != '\0');  // a token is installed: protected requests need it

    const esp_app_desc_t *me = esp_app_get_description();
    cJSON *upd = cJSON_AddObjectToObject(root, "updater");
    cJSON_AddStringToObject(upd, "version", me->version);
    cJSON_AddStringToObject(upd, "idf", me->idf_ver);
    cJSON_AddStringToObject(upd, "running_because", why_running());
    cJSON_AddStringToObject(upd, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddNumberToObject(root, "coredump", coredump_size());  // bytes of a crash dump waiting, 0 = none

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);   // real chip size (not the image header's setting)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof mac_str, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *c = cJSON_AddObjectToObject(root, "chip");
    cJSON_AddStringToObject(c, "target", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(c, "image_chip_id", CONFIG_IDF_FIRMWARE_CHIP_ID);  // must match byte 12 of the app header
    cJSON_AddNumberToObject(c, "revision", chip.revision);                     // e.g. 2 = v0.2
    cJSON_AddNumberToObject(c, "flash_size", flash_size);
    cJSON_AddStringToObject(c, "mac", mac_str);
    cJSON_AddStringToObject(root, "mac", mac_str);  // also top-level, same place as in the app's /info

    cJSON *parts = cJSON_AddArrayToObject(root, "partitions");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "label", p->label);
        cJSON_AddNumberToObject(e, "type", p->type);
        cJSON_AddNumberToObject(e, "subtype", p->subtype);
        cJSON_AddNumberToObject(e, "offset", p->address);
        cJSON_AddNumberToObject(e, "size", p->size);
        cJSON_AddItemToArray(parts, e);
    }
    esp_partition_iterator_release(it);

    cJSON *app_json = cJSON_AddObjectToObject(root, "app");
    const esp_partition_t *app = app_slot();
    esp_app_desc_t desc;
    bool installed = app && esp_ota_get_partition_description(app, &desc) == ESP_OK;
    cJSON_AddBoolToObject(app_json, "installed", installed);
    if (installed) {
        cJSON_AddStringToObject(app_json, "project", desc.project_name);
        cJSON_AddStringToObject(app_json, "version", desc.version);
        cJSON_AddStringToObject(app_json, "idf", desc.idf_ver);
        cJSON_AddStringToObject(app_json, "built", desc.date);
        cJSON_AddStringToObject(app_json, "state", app_state_str(app));
    }
    if (app) cJSON_AddNumberToObject(app_json, "slot_size", app->size);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json ? json : "{}");
    cJSON_free(json);
    return err;
}

/* ---------- Wi-Fi: reconnect forever, a board that gives up can't be reached ---------- */

// WIFI_REASON_* (esp_wifi_types_generic.h) in words: the updater's settings are fixed at install time,
// so the serial log is the only place to learn why it can't get on the network.
static const char *wifi_hint(int reason)
{
    switch (reason) {
    case 201:                            return "network not found: wrong SSID, router off, or 5 GHz-only?";
    case 2: case 15: case 202: case 204: return "wrong password?";
    case 210: case 211:                  return "the network's security mode isn't accepted (WPA3-only?)";
    case 212:                            return "signal too weak";
    default:                             return "can't connect";
    }
}

static TickType_t s_failing_since;  // first failed attempt of the current outage, 0 = connected
static bool s_advised;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Retries come every few seconds: log when the reason changes, else at most every 30 s.
        // Not for ASSOC_LEAVE (8): that's us leaving, e.g. Wi-Fi stopping for the restart after an upload.
        const wifi_event_sta_disconnected_t *d = data;
        static int last;
        static TickType_t last_at;
        TickType_t now = xTaskGetTickCount();
        if (d->reason != WIFI_REASON_ASSOC_LEAVE &&
            (d->reason != last || now - last_at >= pdMS_TO_TICKS(30000))) {
            ESP_LOGW(TAG, "Wi-Fi '%s': %s (reason %d)", CONFIG_UPD_WIFI_SSID, wifi_hint(d->reason), d->reason);
            last = d->reason;
            last_at = now;
        }
        // The advice only after 30 s of failing: right after a reset, routers with protected management frames
        // refuse the first attempts for a few seconds (reason 203/205), which is normal.
        if (!s_failing_since) {
            s_failing_since = now | 1;
        } else if (!s_advised && now - s_failing_since >= pdMS_TO_TICKS(30000)) {
            ESP_LOGW(TAG, "still not connected after 30 s: if the Wi-Fi name or password changed, "
                          "reinstall the updater over USB with the new settings");
            s_advised = true;
        }
    }
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_failing_since = 0;
        s_advised = false;
        ESP_LOGI(TAG, "ready at http://" IPSTR " (http://%s.local)", IP2STR(&e->ip_info.ip), s_name);
    }
}

static void wifi_start(void)
{
    // No nvs_flash_init(): Wi-Fi config and PHY calibration are kept out of NVS (sdkconfig.defaults),
    // so the updater never touches the user app's "nvs" partition.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, s_name);  // DHCP hostname = router's client list; best effort (max 32 chars)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_UPD_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_UPD_WIFI_PASSWORD, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // power save off = much faster uploads
}

void app_main(void)
{
    board_name();  // before wifi_start(): it's also the DHCP hostname
    load_token();  // before anything is served
    if (!s_key[0]) {
        ESP_LOGW(TAG, "no airflash token on this board: anyone on this network can flash it and read its crash dumps "
                      "(reinstall the updater over USB to set one)");
    }
    ESP_LOGI(TAG, "updater %s running because: %s (reset: %s)",
             esp_app_get_description()->version, why_running(), reset_reason_str(esp_reset_reason()));

    if (!CONFIG_UPD_WIFI_SSID[0]) {
        ESP_LOGE(TAG, "no Wi-Fi set: idf.py menuconfig → airflash, then flash the updater again over USB");
        return;  // nothing to serve without a network; the chip stays up, idle
    }
    wifi_start();

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(s_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_airflash", "_tcp", 80, NULL, 0);  // what `airflash find` asks for (same as the component)

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.stack_size = 8192;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &hcfg));
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/update", .method = HTTP_POST, .handler = update_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/run",    .method = HTTP_POST, .handler = run_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/info",   .method = HTTP_GET,  .handler = info_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/nonce",  .method = HTTP_GET,  .handler = nonce_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_GET,    .handler = coredump_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_DELETE, .handler = coredump_delete});
}
