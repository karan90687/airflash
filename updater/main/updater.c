/*
 * Factory updater: lives in the `factory` partition and is never overwritten by OTA.
 *
 * The bootloader only runs it when there is no user app to boot:
 *   - fresh install (otadata blank, ota_0 empty)
 *   - the app in ota_0 reset before confirming itself (rollback → otadata says ABORTED)
 *   - recovery pin held at reset, or otadata erased (over USB, or later by the app hook)
 *
 * HTTP API (port 80):
 *   POST /update  body = app .bin  → write ota_0, verify, boot it
 *   POST /run                      → boot the app already in ota_0 (e.g. after using the recovery pin)
 *   GET  /info                     → JSON: updater, chip, flash, partitions, installed app, why we're running
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"

static const char *TAG = "updater";

/* ---------- helpers ---------- */

static bool authorized(httpd_req_t *req)
{
    if (!CONFIG_UPD_TOKEN[0]) return true;
    char tok[64];
    return httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof tok) == ESP_OK &&
           strcmp(tok, CONFIG_UPD_TOKEN) == 0;
}

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
    if (esp_ota_get_state_partition(app, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID)) {
        return "app_rolled_back";               // app reset/crashed before confirming
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

static esp_err_t update_post(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

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

    char *buf = malloc(4096);
    int left = req->content_len, timeouts = 0;
    while (left > 0 && buf) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) break;
        // the first write also checks the image magic byte (0xE9)
        if ((err = esp_ota_write(ota, buf, r)) != ESP_OK) break;
        left -= r;
    }
    free(buf);
    if (left > 0) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "upload aborted, %d bytes missing (%s)", left, esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   err == ESP_ERR_OTA_VALIDATE_FAILED ? "not an app image" : "upload failed");
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
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

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
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *me = esp_app_get_description();
    cJSON *upd = cJSON_AddObjectToObject(root, "updater");
    cJSON_AddStringToObject(upd, "version", me->version);
    cJSON_AddStringToObject(upd, "idf", me->idf_ver);
    cJSON_AddStringToObject(upd, "running_because", why_running());
    cJSON_AddStringToObject(upd, "reset_reason", reset_reason_str(esp_reset_reason()));

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

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "ready at http://" IPSTR " (http://%s.local)", IP2STR(&e->ip_info.ip), CONFIG_UPD_HOSTNAME);
    }
}

static void wifi_start(void)
{
    // No nvs_flash_init(): Wi-Fi config and PHY calibration are kept out of NVS (sdkconfig.defaults),
    // so the updater never touches the user app's "nvs" partition.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
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
    ESP_LOGI(TAG, "updater %s running because: %s (reset: %s)",
             esp_app_get_description()->version, why_running(), reset_reason_str(esp_reset_reason()));

    wifi_start();

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(CONFIG_UPD_HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.stack_size = 8192;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &hcfg));
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/update", .method = HTTP_POST, .handler = update_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/run",    .method = HTTP_POST, .handler = run_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/info",   .method = HTTP_GET,  .handler = info_get});
}
