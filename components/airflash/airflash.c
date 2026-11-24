/*
 * airflash companion component: lives inside the user's app (the updater in `factory` does the flashing).
 *
 *  - captures every ESP_LOGx line (and esp_rom_printf output, e.g. the task watchdog) from before app_main
 *    into a RAM ring buffer that SURVIVES software resets, panics and watchdog resets (not power-off)
 *  - streams it over TCP:   nc <ip> 3333     or   airflash.py monitor / logs
 *    up to AIRFLASH_LOG_CLIENTS viewers; a viewer may resume with "airflash <id> <offset>\n" (no duplicates)
 *  - HTTP control (port 8080):  GET /info, POST /reboot-to-updater, GET + DELETE /coredump
 *  - after a crash: logs the reset reason + crash summary (task, PC, backtrace) on the next boot
 *  - confirms the app once it's up, so the bootloader doesn't roll back to the updater
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_core_dump.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "airflash.h"

static const char *TAG = "airflash";
#define AIRFLASH_VERSION "0.3.0"

/* ---------- log store: a ring buffer in internal RAM that survives resets ---------- */

#define LOG_BUF_SIZE CONFIG_AIRFLASH_LOG_BUF_SIZE
#define LOG_MAGIC 0xA1F1A5E4

// .noinit: startup never clears it, so after a software/panic/watchdog reset the old contents are still there.
// Internal RAM on purpose: PSRAM sits behind a write-back cache that a reset throws away (the newest lines).
typedef struct {
    uint32_t magic;
    uint32_t app;    // first 4 bytes of this build's ELF SHA-256: another build never trusts the buffer
    uint32_t id;     // random; changes whenever the buffer starts over (viewers use it to resume)
    uint32_t boots;  // boots since the buffer started over
    uint32_t head;   // write index (== total % LOG_BUF_SIZE)
    uint64_t total;  // bytes logged since the buffer started over (offsets continue across reboots)
    char data[LOG_BUF_SIZE];
} log_store_t;

static __NOINIT_ATTR log_store_t s_log;
static bool s_capturing;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;  // spinlock: works before the scheduler starts
static vprintf_like_t s_orig_vprintf = vprintf;

static uint32_t app_tag(void)
{
    const uint8_t *s = esp_app_get_description()->app_elf_sha256;
    return s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
}

// Append n bytes. Caller holds s_log_lock.
static void ring_put(const char *p, size_t n)
{
    if (n > LOG_BUF_SIZE) {  // only the last LOG_BUF_SIZE bytes can be kept
        size_t skip = n - LOG_BUF_SIZE;
        p += skip;
        s_log.total += skip;
        s_log.head = (s_log.head + skip) % LOG_BUF_SIZE;
        n = LOG_BUF_SIZE;
    }
    size_t first = MIN(n, LOG_BUF_SIZE - s_log.head);
    memcpy(s_log.data + s_log.head, p, first);
    memcpy(s_log.data, p + first, n - first);
    s_log.head = (s_log.head + n) % LOG_BUF_SIZE;
    s_log.total += n;
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_orig_vprintf(fmt, ap);  // still print to USB/UART (or the project's own hook)

    // format outside the lock, copy inside it (the lock disables interrupts, so keep it short)
    // ponytail: lines over 191 chars are cut in the stream (USB still gets them whole)
    char line[192];
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > (int)sizeof line - 1) {
        n = sizeof line - 1;
        line[n - 1] = '\n';
    }
    if (n > 0) {
        portENTER_CRITICAL_SAFE(&s_log_lock);
        ring_put(line, n);
        portEXIT_CRITICAL_SAFE(&s_log_lock);
    }
    return ret;
}

#if CONFIG_AIRFLASH_ROM_LOGS
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "xtensa/config/core-isa.h"
#endif

// True inside a level-4+ interrupt handler: the panic handler of the interrupt watchdog runs there.
// Read-only (rsr.ps): every ESP-IDF lock, and even xPortInIsrContext(), sets the level to 3, which from level 4
// re-enables the still-pending watchdog interrupt → a 2nd panic before the core dump is written.
static inline __attribute__((always_inline)) bool in_high_level_handler(void)
{
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    uint32_t ps;
    __asm__ volatile("rsr.ps %0" : "=a"(ps));
    return (ps & 0xF) > XCHAL_EXCM_LEVEL;
#else
    return false;  // ponytail: RISC-V chips not tested yet (stage 6)
#endif
}

// esp_rom_printf's 2nd output channel: ESP_EARLY_LOGx, the task watchdog report, core dump progress.
// Runs in odd places (even inside the panic handler, with the other core stopped), hence IRAM + try-lock:
// if the lock can't be had quickly, the character is dropped instead of hanging the panic.
static void IRAM_ATTR rom_putc(char c)
{
    bool high = in_high_level_handler();  // there everything else is frozen: write without a lock
    if (!high && portTRY_ENTER_CRITICAL_SAFE(&s_log_lock, 10000) != pdPASS) return;
    s_log.data[s_log.head] = c;
    if (++s_log.head == LOG_BUF_SIZE) s_log.head = 0;
    s_log.total++;
    if (!high) portEXIT_CRITICAL_SAFE(&s_log_lock);
}
#endif

void airflash_capture_logs(void)
{
    if (s_capturing) return;
    s_capturing = true;

    if (s_log.magic == LOG_MAGIC && s_log.app == app_tag() && s_log.head < LOG_BUF_SIZE) {
        // same build rebooted (crash, watchdog, esp_restart): keep the lines from before
        // ponytail: if the updater ran in between and left magic + tag intact, old bytes may be garbled
        s_log.boots++;
        static const char mark[] = "\n[airflash] ---- reboot ----\n";
        ring_put(mark, sizeof mark - 1);
    } else {  // power-on, deep sleep, another build: start over
        s_log.magic = LOG_MAGIC;
        s_log.app = app_tag();
        s_log.id = esp_random() | 1;  // never 0: 0 means "no id yet" for viewers
        s_log.boots = 1;
        s_log.head = 0;
        s_log.total = 0;
    }
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
#if CONFIG_AIRFLASH_ROM_LOGS
    esp_rom_install_channel_putc(2, rom_putc);
#endif
}

#if CONFIG_AIRFLASH_EARLY_LOGS
// Runs during startup (global constructors), before app_main(), so the stream starts at the app's first line.
__attribute__((constructor)) static void airflash_early_logs(void)
{
    airflash_capture_logs();
}
#endif

static uint64_t log_oldest(void)
{
    return s_log.total > LOG_BUF_SIZE ? s_log.total - LOG_BUF_SIZE : 0;
}

// Copy up to `max` bytes starting at *pos, WITHOUT advancing past them (the caller advances by what it sent).
// If *pos was already overwritten, it jumps to the oldest byte and *lost says how many bytes were skipped.
static size_t log_peek(uint64_t *pos, char *out, size_t max, uint64_t *lost)
{
    portENTER_CRITICAL(&s_log_lock);
    uint64_t total = s_log.total, oldest = log_oldest();
    *lost = *pos < oldest ? oldest - *pos : 0;
    if (*pos < oldest) *pos = oldest;
    size_t n = MIN(total - *pos, max);
    size_t at = (s_log.head + LOG_BUF_SIZE - (size_t)(total - *pos)) % LOG_BUF_SIZE;  // index of *pos
    size_t first = MIN(n, LOG_BUF_SIZE - at);
    memcpy(out, s_log.data + at, first);
    memcpy(out + first, s_log.data, n - first);
    portEXIT_CRITICAL(&s_log_lock);
    return n;
}

/* ---------- TCP log stream: up to AIRFLASH_LOG_CLIENTS viewers, each with its own position ---------- */

#define MAX_VIEWERS CONFIG_AIRFLASH_LOG_CLIENTS
#define HELLO_WAIT_MS 300

typedef struct {
    int sock;            // -1 = free slot
    uint64_t pos;        // next byte of the log to send to this viewer
    bool waiting;        // still waiting (up to HELLO_WAIT_MS) for an optional resume hello
    bool framed;         // sent a hello: never insert text into its stream, just close on a gap
    TickType_t until;
    char hello[48];
    uint8_t hello_len;
} viewer_t;

static void viewer_close(viewer_t *v)
{
    close(v->sock);
    v->sock = -1;
}

// "airflash <id hex> <offset hex>\n" → resume there if it's the same log and still buffered, else the oldest byte.
// Reply "airflash <id> <start>\n" so the viewer knows where the stream really starts.
static void viewer_hello(viewer_t *v)
{
    unsigned long id = 0;
    char off_hex[24] = "";
    v->waiting = false;
    if (sscanf(v->hello, "airflash %lx %23s", &id, off_hex) != 2) return;  // not a hello: plain viewer
    uint64_t want = strtoull(off_hex, NULL, 16);

    portENTER_CRITICAL(&s_log_lock);
    uint64_t total = s_log.total, oldest = log_oldest();
    uint32_t log_id = s_log.id;
    portEXIT_CRITICAL(&s_log_lock);

    v->pos = (id == log_id && want >= oldest && want <= total) ? want : oldest;
    v->framed = true;
    char reply[48];  // 64-bit value as two 32-bit halves: printf may not support %llx (newlib nano)
    int m = snprintf(reply, sizeof reply, "airflash %08lx %08lx%08lx\n", (unsigned long)log_id,
                     (unsigned long)(v->pos >> 32), (unsigned long)(v->pos & 0xFFFFFFFF));
    if (send(v->sock, reply, m, MSG_DONTWAIT) != m) viewer_close(v);
}

// Send as much as the viewer's socket accepts right now, without ever blocking the other viewers.
static void viewer_pump(viewer_t *v)
{
    static char buf[1024];
    for (int round = 0; round < 4; round++) {  // at most ~4 KB per viewer per loop: fair to the others
        uint64_t lost;
        size_t n = log_peek(&v->pos, buf, sizeof buf, &lost);
        if (lost) {
            if (v->framed) {  // it knows its offset: let it reconnect and report the gap itself
                viewer_close(v);
                return;
            }
            char note[96];
            int m = snprintf(note, sizeof note, "\n[airflash: %lu bytes of logs were overwritten before they could be sent]\n", (unsigned long)lost);
            send(v->sock, note, m, MSG_DONTWAIT);  // best effort
        }
        if (n == 0) return;
        int sent = send(v->sock, buf, n, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) viewer_close(v);  // gone
            return;                                                        // full: try again next loop
        }
        v->pos += sent;
        if ((size_t)sent < n) return;  // socket buffer full for now
    }
}

static void log_server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int yes = 1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_AIRFLASH_LOG_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (ls < 0 || setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) ||
        bind(ls, (struct sockaddr *)&addr, sizeof addr) || listen(ls, 2)) {
        ESP_LOGE(TAG, "log stream: can't listen on port %d", CONFIG_AIRFLASH_LOG_PORT);
        vTaskDelete(NULL);
    }

    viewer_t viewers[MAX_VIEWERS];
    for (int i = 0; i < MAX_VIEWERS; i++) viewers[i].sock = -1;

    for (;;) {
        // wait up to 50 ms for: a new viewer, a hello, or input/close on an existing viewer
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        int maxfd = ls;
        for (int i = 0; i < MAX_VIEWERS; i++) {
            if (viewers[i].sock < 0) continue;
            FD_SET(viewers[i].sock, &rfds);
            maxfd = MAX(maxfd, viewers[i].sock);
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 50 * 1000};
        if (select(maxfd + 1, &rfds, NULL, NULL, &tv) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (FD_ISSET(ls, &rfds)) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_VIEWERS && slot < 0; i++) {
                    if (viewers[i].sock < 0) slot = i;
                }
                if (slot < 0) {
                    static const char busy[] = "[airflash: too many log viewers connected, try again later]\n";
                    send(c, busy, sizeof busy - 1, MSG_DONTWAIT);
                    close(c);
                } else {
                    portENTER_CRITICAL(&s_log_lock);
                    uint64_t oldest = log_oldest();
                    portEXIT_CRITICAL(&s_log_lock);
                    viewers[slot] = (viewer_t){
                        .sock = c, .pos = oldest, .waiting = true,  // plain viewers get the whole buffer
                        .until = xTaskGetTickCount() + pdMS_TO_TICKS(HELLO_WAIT_MS),
                    };
                }
            }
        }

        for (int i = 0; i < MAX_VIEWERS; i++) {
            viewer_t *v = &viewers[i];
            if (v->sock < 0) continue;
            if (FD_ISSET(v->sock, &rfds)) {  // readable: a hello, something it typed (ignored), or it closed
                char in[32];
                int r = recv(v->sock, in, sizeof in, MSG_DONTWAIT);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    viewer_close(v);
                    continue;
                }
                if (r > 0 && v->waiting) {
                    size_t room = sizeof v->hello - 1 - v->hello_len;
                    memcpy(v->hello + v->hello_len, in, MIN((size_t)r, room));
                    v->hello_len += MIN((size_t)r, room);
                    v->hello[v->hello_len] = '\0';
                    if (strchr(v->hello, '\n') || v->hello_len == sizeof v->hello - 1) viewer_hello(v);
                }
            }
            if (v->sock < 0) continue;
            if (v->waiting && (int32_t)(xTaskGetTickCount() - v->until) >= 0) v->waiting = false;  // no hello
            if (!v->waiting) viewer_pump(v);
        }
    }
}

/* ---------- crash report on the boot after a crash ---------- */

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power_on";
    case ESP_RST_EXT:      return "external_pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_USB:      return "usb";
    default:               return "other";
    }
}

static void report_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "boot #%lu, reset reason: %s", (unsigned long)s_log.boots, reset_name(r));
    if (r != ESP_RST_PANIC && r != ESP_RST_INT_WDT && r != ESP_RST_TASK_WDT && r != ESP_RST_WDT) return;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *s = malloc(sizeof *s);
    if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        ESP_LOGE(TAG, "the last boot crashed in task '%s' at PC 0x%08lx", s->exc_task, (unsigned long)s->exc_pc);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        char reason[128];
        if (esp_core_dump_get_panic_reason(reason, sizeof reason) == ESP_OK) ESP_LOGE(TAG, "reason: %s", reason);
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        ESP_LOGE(TAG, "exception cause %lu, address 0x%08lx", (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
        for (uint32_t i = 0; i < s->exc_bt_info.depth; i++) {  // one PC per line: idf.py monitor decodes each to file:line
            ESP_LOGE(TAG, "  backtrace #%lu 0x%08lx", (unsigned long)i, (unsigned long)s->exc_bt_info.bt[i]);
        }
#endif
        ESP_LOGE(TAG, "full crash dump: airflash.py coredump <project>  (or: idf.py airflash-coredump)");
    } else {
        ESP_LOGW(TAG, "the last boot crashed, but no crash dump was found");
    }
    free(s);
#else
    ESP_LOGW(TAG, "the last boot crashed; to see where, enable CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH");
#endif
}

/* ---------- HTTP control: /info, /reboot-to-updater, /coredump ---------- */

static bool authorized(httpd_req_t *req)
{
    if (!CONFIG_AIRFLASH_TOKEN[0]) return true;
    char tok[64];
    return httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof tok) == ESP_OK &&
           strcmp(tok, CONFIG_AIRFLASH_TOKEN) == 0;
}

static const char *app_state(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) return "unknown";
    switch (st) {
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    default:                         return "other";
    }
}

static size_t coredump_size(void)
{
    size_t addr, size;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK ? size : 0;
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
    const esp_app_desc_t *d = esp_app_get_description();
    char json[400];
    snprintf(json, sizeof json,
             "{\"role\":\"app\",\"airflash\":\"%s\",\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\","
             "\"state\":\"%s\",\"log_port\":%d,\"reset_reason\":\"%s\",\"boot\":%lu,\"coredump\":%u}",
             AIRFLASH_VERSION, d->project_name, d->version, d->idf_ver, app_state(), CONFIG_AIRFLASH_LOG_PORT,
             reset_name(esp_reset_reason()), (unsigned long)s_log.boots, (unsigned)coredump_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// The hook: point the bootloader at the factory updater and restart.
static esp_err_t reboot_to_updater_post(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
    const esp_partition_t *updater = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!updater) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no updater installed (no factory partition)");

    esp_err_t err = esp_ota_set_boot_partition(updater);  // for factory this erases otadata
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

    ESP_LOGI(TAG, "rebooting into the updater");
    httpd_resp_sendstr(req, "OK, rebooting into the updater\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    s_log.magic = 0;  // the updater reuses this RAM: don't trust it when an app boots again
    esp_restart();
    return ESP_OK;
}

// Raw bytes of the core dump partition (what `idf.py coredump-info -c <file>` reads), then DELETE to clear it.
static esp_err_t coredump_get(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
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
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad token");
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return httpd_resp_sendstr(req, "OK, core dump erased\n");
}

/* ---------- optional built-in Wi-Fi (for apps that don't connect themselves) ---------- */

#if CONFIG_AIRFLASH_WIFI_ENABLE
static EventGroupHandle_t s_wifi_ev;
#define GOT_IP BIT0

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_wifi_ev, GOT_IP);
        esp_wifi_connect();  // retry forever: a board that gives up can't be reached
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
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
    strlcpy((char *)wc.sta.ssid, CONFIG_AIRFLASH_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_AIRFLASH_WIFI_PASSWORD, sizeof wc.sta.password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    return ESP_OK;
}
#endif

/* ---------- public entry point ---------- */

esp_err_t airflash_start(void)
{
    airflash_capture_logs();  // no-op if the early hook already ran
    report_boot();

#if CONFIG_AIRFLASH_WIFI_ENABLE
    ESP_RETURN_ON_ERROR(wifi_start(), TAG, "wifi");
#endif

    esp_err_t err = mdns_init();  // INVALID_STATE = the project already started mDNS, reuse it
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    mdns_hostname_set(CONFIG_AIRFLASH_HOSTNAME);

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = CONFIG_AIRFLASH_HTTP_PORT;
    hcfg.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;  // must differ from any httpd the project runs itself
    hcfg.max_open_sockets = 2;                     // control requests are rare; leave lwIP sockets to the app
    httpd_handle_t srv;
    ESP_RETURN_ON_ERROR(httpd_start(&srv, &hcfg), TAG, "httpd start (port %d in use?)", CONFIG_AIRFLASH_HTTP_PORT);
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/info", .method = HTTP_GET, .handler = info_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/reboot-to-updater", .method = HTTP_POST, .handler = reboot_to_updater_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_GET, .handler = coredump_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_DELETE, .handler = coredump_delete});

    if (xTaskCreate(log_server_task, "airflash_log", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "airflash %s in %s %s: logs on tcp port %d, control on http port %d",
             AIRFLASH_VERSION, app->project_name, app->version, CONFIG_AIRFLASH_LOG_PORT, CONFIG_AIRFLASH_HTTP_PORT);

    // Rollback: a freshly installed app must confirm itself, else the bootloader goes back to the updater.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }
#if CONFIG_AIRFLASH_WIFI_ENABLE
    if (!(xEventGroupWaitBits(s_wifi_ev, GOT_IP, false, true, pdMS_TO_TICKS(CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S * 1000)) & GOT_IP)) {
        ESP_LOGE(TAG, "new app got no Wi-Fi in %ds, rolling back to the updater", CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
#endif
#if CONFIG_AIRFLASH_AUTO_MARK_VALID
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new app confirmed (valid)");
#else
    ESP_LOGW(TAG, "new app pending verify: call esp_ota_mark_app_valid_cancel_rollback() once healthy");
#endif
    return ESP_OK;
}
