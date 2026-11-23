#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "remote_flash.h"

static const char *TAG = "remote_flash";

/* ---------- log capture: every ESP_LOGx also goes into a RAM ring buffer ---------- */

#define LOG_BUF_SIZE CONFIG_RF_LOG_BUF_SIZE
static char *s_log;                 // PSRAM if the board has it, else internal RAM
static size_t s_log_total;          // bytes ever logged; ring position = total % size
static SemaphoreHandle_t s_log_mtx;
static vprintf_like_t s_orig_vprintf;

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_orig_vprintf(fmt, ap);  // still print to USB/UART (or the project's own hook)

    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        static char line[256];
        int n = vsnprintf(line, sizeof line, fmt, copy);
        if (n > (int)sizeof line - 1) n = sizeof line - 1;
        for (int i = 0; i < n; i++) s_log[(s_log_total + i) % LOG_BUF_SIZE] = line[i];
        s_log_total += n > 0 ? n : 0;
        xSemaphoreGive(s_log_mtx);
    }
    va_end(copy);
    return ret;
}

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

void remote_flash_capture_logs(void)
{
    if (s_log_mtx) return;
    s_log = big_alloc(LOG_BUF_SIZE);
    if (!s_log) return;
    s_log_mtx = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}

/* ---------- HTTP server: POST /update (OTA), GET /logs?from=N ---------- */

static bool authorized(httpd_req_t *req)
{
    if (!CONFIG_RF_TOKEN[0]) return true;
    char tok[64];
    return httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof tok) == ESP_OK &&
           strcmp(tok, CONFIG_RF_TOKEN) == 0;
}

// Returns log bytes from offset `from` onward; header X-Next = offset to ask for next time.
// If `from` is ahead of the device (it rebooted) or too old (overwritten), send everything kept.
static esp_err_t logs_get(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

    size_t from = 0;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "from", v, sizeof v) == ESP_OK) {
        from = strtoul(v, NULL, 10);
    }

    char *buf = s_log_mtx ? big_alloc(LOG_BUF_SIZE) : NULL;
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    xSemaphoreTake(s_log_mtx, portMAX_DELAY);  // no logging while held
    size_t total = s_log_total;
    size_t oldest = total > LOG_BUF_SIZE ? total - LOG_BUF_SIZE : 0;
    if (from < oldest || from > total) from = oldest;
    size_t n = total - from;
    for (size_t i = 0; i < n; i++) buf[i] = s_log[(from + i) % LOG_BUF_SIZE];
    xSemaphoreGive(s_log_mtx);

    char next[16];
    snprintf(next, sizeof next, "%u", (unsigned)total);
    httpd_resp_set_hdr(req, "X-Next", next);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition (see README)");
    if (req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image larger than OTA slot");
    }
    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", (unsigned)req->content_len, part->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

    char *buf = malloc(4096);
    int left = req->content_len, timeouts = 0;
    while (left > 0 && buf) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0 || esp_ota_write(ota, buf, r) != ESP_OK) break;
        left -= r;
    }
    free(buf);
    if (left > 0) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA: upload aborted, %d bytes missing", left);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }

    // esp_ota_end validates the image (checksum, chip id) before we switch to it
    if ((err = esp_ota_end(ota)) != ESP_OK || (err = esp_ota_set_boot_partition(part)) != ESP_OK) {
        ESP_LOGE(TAG, "OTA: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA: done, rebooting into %s", part->label);
    httpd_resp_sendstr(req, "OK, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---------- optional built-in Wi-Fi ---------- */

#if CONFIG_RF_WIFI_ENABLE
static EventGroupHandle_t s_wifi_ev;
#define GOT_IP BIT0

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_wifi_ev, GOT_IP);
        esp_wifi_connect();  // retry forever: a board that gives up can't be reflashed remotely
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR ", reachable at http://%s.local", IP2STR(&e->ip_info.ip), CONFIG_RF_HOSTNAME);
        xEventGroupSetBits(s_wifi_ev, GOT_IP);
    }
}

static esp_err_t wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    s_wifi_ev = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_RF_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_RF_WIFI_PASSWORD, sizeof wc.sta.password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    esp_wifi_set_ps(WIFI_PS_NONE);  // power save off = much faster uploads
    return ESP_OK;
}
#endif

/* ---------- public entry point ---------- */

esp_err_t remote_flash_start(void)
{
    remote_flash_capture_logs();

#if CONFIG_RF_WIFI_ENABLE
    ESP_RETURN_ON_ERROR(wifi_start(), TAG, "wifi");
#endif

    esp_err_t err = mdns_init();  // INVALID_STATE = project already started mDNS, reuse it
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    mdns_hostname_set(CONFIG_RF_HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_RF_HTTP_PORT, NULL, 0);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = CONFIG_RF_HTTP_PORT;
    hcfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;  // must differ from any httpd the project runs itself
    hcfg.stack_size = 8192;
    httpd_handle_t srv;
    ESP_RETURN_ON_ERROR(httpd_start(&srv, &hcfg), TAG, "httpd start (port %d in use?)", CONFIG_RF_HTTP_PORT);
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/update", .method = HTTP_POST, .handler = update_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/logs", .method = HTTP_GET, .handler = logs_get});

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s running from %s (built %s %s), OTA on port %d",
             app->project_name, app->version, running->label, app->date, app->time, CONFIG_RF_HTTP_PORT);

    // Rollback: a freshly OTA'd image must prove it works, else the bootloader goes back to the old one.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }
#if CONFIG_RF_WIFI_ENABLE
    if (!(xEventGroupWaitBits(s_wifi_ev, GOT_IP, false, true, pdMS_TO_TICKS(CONFIG_RF_ROLLBACK_TIMEOUT_S * 1000)) & GOT_IP)) {
        ESP_LOGE(TAG, "new firmware got no Wi-Fi in %ds, rolling back", CONFIG_RF_ROLLBACK_TIMEOUT_S);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
#endif
#if CONFIG_RF_AUTO_MARK_VALID
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new firmware marked valid");
#else
    ESP_LOGW(TAG, "new firmware pending verify: call esp_ota_mark_app_valid_cancel_rollback() once healthy");
#endif
    return ESP_OK;
}
