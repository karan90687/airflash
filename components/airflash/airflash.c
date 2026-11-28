/*
 * airflash companion component: lives inside the user's app (the updater in `factory` does the flashing).
 *
 *  - captures every ESP_LOGx line, esp_rom_printf output (startup lines, task watchdog, core dump progress) and
 *    the panic handler's own text, from the app's first lines, into a RAM ring buffer that SURVIVES software
 *    resets, panics and watchdog resets (not power-off); the newest part also goes into the core dump
 *  - streams it over TCP (port 3333) to idf.py airflash-monitor / airflash.py logs (not plain nc): up to
 *    AIRFLASH_LOG_CLIENTS viewers, each answers a challenge first, then resumes exactly where it stopped
 *  - HTTP control (port 8080):  GET /info, GET /nonce, POST /reboot-to-updater, GET + DELETE /coredump
 *  - pairing: the updater install writes a token into the `upd_cfg` partition; with one there, the log stream,
 *    /reboot-to-updater and /coredump need proof of it (HMAC of a fresh nonce: the token never crosses the
 *    network). Lock only, nothing is encrypted. No token (or an unreadable one) = open mode, never an error.
 *  - after a crash: logs the reset reason on the next boot (+ a crash summary if the panic text wasn't captured)
 *  - first boot after a flash: confirms the app once it's on the network; if it isn't after
 *    AIRFLASH_ROLLBACK_TIMEOUT_S, aborts with the reason, so the bootloader rolls back to the updater
 *  - parks an app that crashes right after every boot in the updater (AIRFLASH_CRASH_LOOP_COUNT)
 *
 * Rule: airflash must never be the reason an app doesn't work. Whatever can't work here (older IDF, missing
 * config, an error) is logged and skipped, airflash_start() never fails; the deadline and the crash-loop guard are
 * the only interventions.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_rom_crc.h"
#include "esp_idf_version.h"
#include "esp_private/startup_internal.h"
#include "esp_timer.h"
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
#include "esp_mac.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"
#include "airflash.h"

// Weak: a plain reference to WIFI_EVENT would drag the whole Wi-Fi driver into Ethernet-only apps.
// An app that uses Wi-Fi has it; otherwise &WIFI_EVENT is NULL and airflash just doesn't watch Wi-Fi.
extern esp_event_base_t const WIFI_EVENT __attribute__((weak));

static const char *TAG = "airflash";
#define AIRFLASH_VERSION "0.1.0"

static bool is_crash(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

/* ---------- log store: a ring buffer in internal RAM that survives resets ---------- */

#define LOG_BUF_SIZE CONFIG_AIRFLASH_LOG_BUF_SIZE
#define LOG_MAGIC 0xA1F1A5E4

// .noinit: startup never clears it, so after a software/panic/watchdog reset the old contents are still there.
// Internal RAM on purpose: PSRAM sits behind a write-back cache that a reset throws away (the newest lines).
typedef struct {
    uint32_t magic;
    uint32_t app;     // first 4 bytes of this build's ELF SHA-256: another build never trusts the buffer
    uint32_t id;      // random; changes whenever the buffer starts over (viewers use it to resume)
    uint32_t boots;   // boots since the buffer started over
    uint32_t head;    // write index (== total % LOG_BUF_SIZE)
    uint32_t panic;   // the panic handler's text of this boot is in the buffer (read on the next boot)
    uint32_t young;   // crash-loop guard: 1 until this boot has lived AIRFLASH_CRASH_LOOP_WINDOW_S
    uint32_t streak;  // crash-loop guard: boots in a row that crashed while young
    uint64_t total;   // bytes logged since the buffer started over (offsets continue across reboots)
    char data[LOG_BUF_SIZE];
} log_store_t;

static __NOINIT_ATTR log_store_t s_log;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;  // spinlock: works before the scheduler starts
static vprintf_like_t s_orig_vprintf = vprintf;
static bool s_store_ready, s_kept, s_prev_panic, s_capturing;
static volatile bool s_in_panic;  // the panic handler runs: the other core is stalled, never wait for a lock

#if CONFIG_AIRFLASH_COREDUMP_LOG_TAIL > 0
#define TAIL_SIZE CONFIG_AIRFLASH_COREDUMP_LOG_TAIL
// The newest log bytes once more, in the section the core dump always saves (COREDUMP_DRAM_ATTR = .dram2.coredump,
// placed inside .dram0.data). s_log is gone once the updater runs after a rollback (it reuses the RAM); the core
// dump stays in flash. Initialized data = reloaded (zeroed) at every boot, so this holds the crashed boot only.
// airflash.py finds it by searching the raw dump for the magic. Little-endian, no padding.
typedef struct {
    char magic[8];   // "AFTAIL01", set at boot
    uint32_t size;   // sizeof data
    uint32_t head;   // next write index
    uint64_t total;  // bytes ever written: if < size the log is data[0:head], else data[head:] + data[:head]
    char data[TAIL_SIZE];
} log_tail_t;
_Static_assert(offsetof(log_tail_t, data) == 24, "airflash.py reads the tail's data at offset 24");
static COREDUMP_DRAM_ATTR log_tail_t s_tail;
#else
#define TAIL_SIZE 0
#endif

static uint32_t app_tag(void)
{
    const uint8_t *s = esp_app_get_description()->app_elf_sha256;
    return s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
}

// Append n bytes to the ring (and the core dump's tail). Byte by byte, IRAM, no libc: it also runs inside the panic
// handler and with the flash cache off. Caller holds s_log_lock, or nothing else can run (panic, early startup).
// head and total move together per byte, so a reset (or a stalled core) mid-line never leaves them out of step.
static void IRAM_ATTR log_put(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_log.data[s_log.head] = p[i];
        if (++s_log.head == LOG_BUF_SIZE) s_log.head = 0;
        s_log.total++;
#if TAIL_SIZE
        s_tail.data[s_tail.head] = p[i];
        if (++s_tail.head == TAIL_SIZE) s_tail.head = 0;
        s_tail.total++;
#endif
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_orig_vprintf(fmt, ap);  // still print to USB/UART (or the project's own hook)

    // format outside the lock, copy inside it (the lock disables interrupts, so keep it short)
    // It formats into a 192-byte buffer on the caller's stack: every ESP_LOGx call needs ~256 B more stack in its task.
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
        log_put(line, n);
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

// esp_rom_printf's 2nd output channel: ESP_EARLY_LOGx (the startup lines), the task watchdog report, core dump
// progress. Runs in odd places (even inside the panic handler, with the other core stopped), hence IRAM + try-lock:
// if the lock can't be had quickly, the character is dropped instead of hanging the panic.
static void IRAM_ATTR rom_putc(char c)
{
    // panic: the stalled core may hold the lock forever. Level-4+ handler: see above. Both: everything else is frozen.
    bool frozen = s_in_panic || in_high_level_handler();
    if (!frozen && portTRY_ENTER_CRITICAL_SAFE(&s_log_lock, 10000) != pdPASS) return;
    log_put(&c, 1);
    if (!frozen) portEXIT_CRITICAL_SAFE(&s_log_lock);
}
#endif

/* ---------- the panic handler's own text (AIRFLASH_PANIC_TEXT) ---------- */

#if AIRFLASH_WRAP_PANIC
// The panic handler prints straight to the UART/USB hardware, past every log hook. CMakeLists.txt links with
// --wrap=X for its print functions: a call to X from ANOTHER object file (registers, backtrace, details) lands in
// __wrap_X below, which copies the text and then calls the real X (__real_X), so the console is untouched.
// Calls inside panic.c itself aren't redirected (the linker only rewires undefined references), so its own lines
// (headline, ELF SHA, "Rebooting...") are rebuilt here from the same data, in the same place.
// Panic context: the other core is stalled, the flash cache may be off → IRAM code, DRAM data, no locks, no libc.
#include "esp_private/panic_internal.h"

void __real_esp_panic_handler(panic_info_t *info);
void __real_panic_restart(void) __attribute__((noreturn));
void __real_panic_print_char(char c);
void __real_panic_print_str(const char *s);
void __real_panic_print_hex(int h);
void __real_panic_print_dec(int d);

static DRAM_ATTR char s_sha[CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1];  // panic.c prints exactly this many hex digits
static panic_info_dump_fn_t s_print_state;

static void IRAM_ATTR panic_putc(char c)
{
    log_put(&c, 1);
}

static void IRAM_ATTR panic_puts(const char *s)
{
    while (*s) panic_putc(*s++);
}

static void IRAM_ATTR put_dec(int d)  // the same 2 characters as panic_print_dec(): tens digit or ' ', ones digit
{
    int n1 = d % 10, n2 = d / 10;
    panic_putc(n2 == 0 ? ' ' : n2 + '0');
    panic_putc(n1 + '0');
}

void IRAM_ATTR __wrap_panic_print_char(char c)
{
    if (s_in_panic) panic_putc(c);
    __real_panic_print_char(c);
}

void IRAM_ATTR __wrap_panic_print_str(const char *s)
{
    if (s_in_panic) panic_puts(s);
    __real_panic_print_str(s);
}

void IRAM_ATTR __wrap_panic_print_hex(int h)  // like panic_print_hex(): 8 lowercase digits, no "0x"
{
    for (int shift = 28; s_in_panic && shift >= 0; shift -= 4) {
        int c = ((uint32_t)h >> shift) & 0xf;
        panic_putc(c < 10 ? '0' + c : 'a' + c - 10);
    }
    __real_panic_print_hex(h);
}

void IRAM_ATTR __wrap_panic_print_dec(int d)
{
    if (s_in_panic) put_dec(d);
    __real_panic_print_dec(d);
}

// panic.c prints "\r\n" right before the processor state and the ELF SHA lines right after it, from inside panic.c.
// Wrapping info->state (registers + backtrace) puts exactly those lines in the buffer, exactly there.
static void IRAM_ATTR state_and_sha(const void *frame)
{
    panic_puts(DRAM_STR("\r\n"));
    s_print_state(frame);
    panic_puts(DRAM_STR("\r\n\r\nELF file SHA256: "));
    panic_puts(s_sha);
    panic_puts(DRAM_STR("\r\n\r\n"));
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    s_in_panic = s_store_ready;  // the port's panic_handler() has already stalled the other core
    if (s_in_panic) {
        s_log.panic = 1;
        // the headline, as esp_panic_handler() is about to print it (for an abort() it's the abort message instead)
        if (g_panic_abort) {
            panic_puts(DRAM_STR("\r\n"));
            if (g_panic_abort_details) panic_puts(g_panic_abort_details);
        } else {
            if (info->reason) {
                panic_puts(DRAM_STR("Guru Meditation Error: Core "));
                put_dec(info->core);
                panic_puts(DRAM_STR(" panic'ed ("));
                panic_puts(info->reason);
                panic_puts(DRAM_STR("). "));
            }
            if (info->description) panic_puts(info->description);
            panic_puts(DRAM_STR("\r\n"));
        }
        if (info->state) {
            s_print_state = info->state;
            info->state = state_and_sha;
        }
    }
    __real_esp_panic_handler(info);
    s_in_panic = false;  // only reached when a debugger is attached: it returns to a breakpoint
}

void IRAM_ATTR __wrap_panic_restart(void)
{
    if (s_in_panic) panic_puts(DRAM_STR("Rebooting...\r\n"));  // panic.c prints it just before calling this
    __real_panic_restart();
}
#endif

/* ---------- start capturing: as early as possible ---------- */

// Pick up the buffer from before the reset, or start over. Runs at the very start of the app, before the heap,
// stdout and FreeRTOS exist: no heap, no locks, no flash API.
static void store_init(void)
{
    if (s_store_ready) return;
#if TAIL_SIZE
    memcpy(s_tail.magic, "AFTAIL01", 8);
    s_tail.size = TAIL_SIZE;
#endif
#if AIRFLASH_WRAP_PANIC
    esp_app_get_elf_sha256(s_sha, sizeof s_sha);  // now: in a panic, flash (where it comes from) may be unreadable
#endif
    s_kept = s_log.magic == LOG_MAGIC && s_log.app == app_tag() && s_log.head < LOG_BUF_SIZE;
    if (s_kept) {  // same build rebooted (crash, watchdog, esp_restart): keep the lines from before
        // ponytail: if the updater ran in between and left magic + tag intact, old bytes may be garbled
        s_log.boots++;
        s_prev_panic = s_log.panic;
        static const char mark[] = "\n[airflash] ---- reboot ----\n";
        log_put(mark, sizeof mark - 1);
    } else {  // power-on, deep sleep, another build: start over
        s_log.magic = LOG_MAGIC;
        s_log.app = app_tag();
        s_log.id = esp_random() | 1;  // never 0: 0 means "no id yet" for viewers
        s_log.boots = 1;
        s_log.head = 0;
        s_log.total = 0;
        s_log.young = 0;
        s_log.streak = 0;
    }
    s_log.panic = 0;
    s_store_ready = true;
}

// esp_rom_printf's channel only: safe from the very first line (no heap, no FreeRTOS).
static void capture_rom(void)
{
    store_init();
#if CONFIG_AIRFLASH_ROM_LOGS
    esp_rom_install_channel_putc(2, rom_putc);
#endif
}

void airflash_capture_logs(void)
{
    capture_rom();  // no-op if the early hook already did it
    if (s_capturing) return;
    s_capturing = true;
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);  // needs the heap: the log lock is created on first use
}

#if CONFIG_AIRFLASH_EARLY_LOGS
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
// Earliest hook: an init function of the CORE startup stage (priority 5), before the startup code prints the CPU
// frequency (10), app info (20), chip revision (21), heap regions (100) and flash chip (130). Those are
// ESP_EARLY_LOGx = esp_rom_printf lines, so the ROM channel catches them; the vprintf hook needs the heap and
// waits for the constructor below. (IDF < 5.3 has no CORE stage for components: the constructor is the first hook.)
ESP_SYSTEM_INIT_FN(airflash_early, CORE, BIT(0), 5)
{
    capture_rom();
    return ESP_OK;  // anything else would abort the boot
}
#endif

// Global constructors run right after the CORE stage (heap and stdout ready), well before app_main().
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

/* ---------- pairing: the board's token, written into `upd_cfg` when the updater was installed ---------- */

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

static void new_nonce(char out[33])
{
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    hex(r, sizeof r, out);
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

/* ---------- TCP log stream: up to AIRFLASH_LOG_CLIENTS viewers, each with its own position ---------- */

#define MAX_VIEWERS CONFIG_AIRFLASH_LOG_CLIENTS
#define HELLO_WAIT_MS 5000  // a viewer that hasn't answered the challenge by then is denied

typedef struct {
    int sock;            // -1 = free slot
    uint64_t pos;        // next byte of the log to send to this viewer
    bool waiting;        // hasn't sent its hello yet
    TickType_t until;
    char nonce[33];      // this connection's challenge, "-" in open mode
    char hello[112];     // "airflash <id> <offset> <hmac>\n": up to 100 chars
    uint8_t hello_len;
} viewer_t;

static void viewer_close(viewer_t *v)
{
    close(v->sock);
    v->sock = -1;
}

static void viewer_deny(viewer_t *v)
{
    static const char denied[] = "airflash-denied\n";
    send(v->sock, denied, sizeof denied - 1, MSG_DONTWAIT);
    viewer_close(v);
}

// The board sent "airflash-auth <nonce>\n"; the viewer answers "airflash <id hex> <offset hex> <hmac>\n",
// hmac = HMAC-SHA256(token, "<nonce>\nlog") ("-" in open mode). Wrong or garbled: "airflash-denied\n" and close.
// Then resume at <offset> if it's the same log and still buffered, else the oldest byte, and reply
// "airflash <id> <start>\n" so the viewer knows where the stream really starts.
static void viewer_hello(viewer_t *v)
{
    unsigned long id = 0;
    char off_hex[24] = "", mac[72] = "", msg[40];
    v->waiting = false;
    snprintf(msg, sizeof msg, "%s\nlog", v->nonce);
    if (sscanf(v->hello, "airflash %lx %23s %71s", &id, off_hex, mac) != 3 || (s_key[0] && !hmac_ok(msg, mac))) {
        viewer_deny(v);
        return;
    }
    uint64_t want = strtoull(off_hex, NULL, 16);

    portENTER_CRITICAL(&s_log_lock);
    uint64_t total = s_log.total, oldest = log_oldest();
    uint32_t log_id = s_log.id;
    portEXIT_CRITICAL(&s_log_lock);

    v->pos = (id == log_id && want >= oldest && want <= total) ? want : oldest;
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
        if (lost) {  // it knows its offset: let it reconnect and report the gap itself
            viewer_close(v);
            return;
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
        ESP_LOGE(TAG, "log stream: can't listen on tcp port %d (%s): already used by the app? change "
                      "CONFIG_AIRFLASH_LOG_PORT. The app and the control port keep running",
                 CONFIG_AIRFLASH_LOG_PORT, strerror(errno));
        if (ls >= 0) close(ls);
        vTaskDelete(NULL);
    }

    static viewer_t viewers[MAX_VIEWERS];  // static: keeps the task's stack for the HMAC
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
                    viewer_t *v = &viewers[slot];
                    *v = (viewer_t){
                        .sock = c, .waiting = true, .nonce = "-",
                        .until = xTaskGetTickCount() + pdMS_TO_TICKS(HELLO_WAIT_MS),
                    };
                    if (s_key[0]) new_nonce(v->nonce);
                    char challenge[48];
                    int m = snprintf(challenge, sizeof challenge, "airflash-auth %s\n", v->nonce);
                    if (send(c, challenge, m, MSG_DONTWAIT) != m) viewer_close(v);
                }
            }
        }

        for (int i = 0; i < MAX_VIEWERS; i++) {
            viewer_t *v = &viewers[i];
            if (v->sock < 0) continue;
            if (FD_ISSET(v->sock, &rfds)) {  // readable: its hello, anything after it (ignored), or it closed
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
            if (v->waiting && (int32_t)(xTaskGetTickCount() - v->until) >= 0) viewer_deny(v);  // no answer
            else if (!v->waiting) viewer_pump(v);
        }
    }
}

/* ---------- crash report on the boot after a crash ---------- */

static size_t coredump_size(void)
{
    size_t addr, size;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK ? size : 0;
}

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

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
// Words for the crash when there's no abort()/assert text: watchdog name, or the CPU exception's name.
static const char *crash_name(esp_reset_reason_t r, const esp_core_dump_summary_t *s)
{
    if (r == ESP_RST_INT_WDT) return "Interrupt wdt timeout";
    if (r == ESP_RST_TASK_WDT) return "Task watchdog timeout";
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    switch (s->ex_info.exc_cause) {
    case 0:  return "IllegalInstruction";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 6:  return "IntegerDivideByZero";
    case 9:  return "LoadStoreAlignment";
    case 20: return "InstrFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    }
#endif
    return "exception";
}

// Fallback when the panic text itself wasn't captured: the same information, rebuilt from the core dump in the
// familiar Guru Meditation layout. esp_log_write = no "E (t) tag:" prefix, still captured.
// idf.py monitor decodes every 0x4... address to file:line.
static bool print_summary(esp_reset_reason_t r)
{
    esp_core_dump_summary_t *s = malloc(sizeof *s);
    bool ok = s && esp_core_dump_get_summary(s) == ESP_OK;
    if (ok) {
        char reason[128] = "";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        esp_core_dump_get_panic_reason(reason, sizeof reason);  // abort()/assert text; empty for plain exceptions
#endif
        if (!reason[0]) snprintf(reason, sizeof reason, "%s", crash_name(r, s));
        esp_log_write(ESP_LOG_ERROR, TAG, "\nGuru Meditation Error (previous boot): task '%s' panic'ed (%s)\n", s->exc_task, reason);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        esp_log_write(ESP_LOG_ERROR, TAG, "PC      : 0x%08lx  EXCCAUSE: 0x%08lx  EXCVADDR: 0x%08lx\n", (unsigned long)s->exc_pc,
                      (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
        char bt[192];
        int n = snprintf(bt, sizeof bt, "Backtrace:");
        for (uint32_t i = 0; i < s->exc_bt_info.depth && n < (int)sizeof bt - 12; i++) {
            n += snprintf(bt + n, sizeof bt - n, " 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
        }
        esp_log_write(ESP_LOG_ERROR, TAG, "%s%s\n\n", bt, s->exc_bt_info.corrupted ? " |<-CORRUPTED" : "");
#endif
    }
    free(s);
    return ok;
}
#endif

static void report_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    ESP_LOGI(TAG, "boot #%lu, reset reason: %s", (unsigned long)s_log.boots, reset_name(r));
    if (!is_crash(r)) return;

    // s_prev_panic: the real panic text is already in the stream, right before the reboot marker
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (s_prev_panic ? coredump_size() > 0 : print_summary(r)) {
        ESP_LOGW(TAG, "full crash dump (registers, all tasks): airflash.py coredump <project>  or  idf.py airflash-coredump");
    } else {
        ESP_LOGW(TAG, "the last boot crashed, but no crash dump was found");
    }
#else
    if (!s_prev_panic) ESP_LOGW(TAG, "the last boot crashed; to see where, enable CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH");
#endif
}

/* ---------- board name: <name>.local, DHCP hostname, "name" in /info ---------- */

static char s_name[64], s_mac[18];  // 63 = longest mDNS label

// Empty hostname setting → "airflash-XXXX" from the MAC's last 2 bytes, so boards on one network don't all
// claim the same .local name. The updater uses the exact same rule, so a board keeps its name in both.
static void board_name(void)
{
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);  // factory MAC from eFuse: works before Wi-Fi is started
    snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    if (CONFIG_AIRFLASH_HOSTNAME[0]) strlcpy(s_name, CONFIG_AIRFLASH_HOSTNAME, sizeof s_name);
    else snprintf(s_name, sizeof s_name, "airflash-%02x%02x", m[4], m[5]);
}

/* ---------- HTTP control: /info, /reboot-to-updater, /coredump ---------- */

// GET /nonce: a challenge for one protected request. Single use, valid 60 s, the newest NONCES are kept.
// Only the httpd task uses the pool (each log viewer gets its own nonce): no lock.
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
    new_nonce(s_nonces[i].hex);
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

// Protected requests carry "X-Airflash-Auth: <nonce>:<hmac>", hmac = HMAC-SHA256(token, "<nonce>\n<METHOD>\n<path>").
// e.g. token 00112233445566778899aabbccddeeff, nonce 0123456789abcdef0123456789abcdef, POST /update
//   → 353e8fa658d26de92fed45ec56838a74ff53f6be6fe63f74fa757eb07ae500f6 (Python's hmac module agrees)
static bool authorized(httpd_req_t *req)
{
    if (!s_key[0]) return true;  // open mode
    char h[100], msg[128];       // h: 32 + ':' + 64
    if (httpd_req_get_hdr_value_str(req, "X-Airflash-Auth", h, sizeof h) != ESP_OK || strlen(h) != 97 || h[32] != ':') {
        return false;
    }
    h[32] = '\0';
    snprintf(msg, sizeof msg, "%s\n%s\n%.*s", h, http_method_str(req->method), (int)strcspn(req->uri, "?"), req->uri);
    return nonce_take(h) && hmac_ok(msg, h + 33);
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

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char name[MDNS_NAME_BUF_LEN];  // the .local name in use right now: the app may set its own at any time
    if (mdns_hostname_get(name) != ESP_OK || !name[0]) strlcpy(name, s_name, sizeof name);
    char json[400];  // longest possible values (31-char app strings, 68-char name) = 391 bytes
    snprintf(json, sizeof json,
             "{\"role\":\"app\",\"airflash\":\"%s\",\"name\":\"%s\",\"mac\":\"%s\",\"project\":\"%s\","
             "\"version\":\"%s\",\"idf\":\"%s\",\"state\":\"%s\",\"log_port\":%d,\"reset_reason\":\"%s\","
             "\"boot\":%lu,\"coredump\":%u,\"auth\":%s}",
             AIRFLASH_VERSION, name, s_mac, d->project_name, d->version, d->idf_ver, app_state(),
             CONFIG_AIRFLASH_LOG_PORT, reset_name(esp_reset_reason()), (unsigned long)s_log.boots,
             (unsigned)coredump_size(), s_key[0] ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// The hook: point the bootloader at the factory updater and restart.
static esp_err_t reboot_to_updater_post(httpd_req_t *req)
{
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);
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
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);
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
    if (!authorized(req)) return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NOT_PAIRED);
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return httpd_resp_sendstr(req, "OK, core dump erased\n");
}

/* ---------- boot checks: crash-loop guard, and the deadline for a freshly flashed app ---------- */

static bool s_started;         // airflash is reachable: airflash_start() got its HTTP server running
static esp_err_t s_start_err;  // why airflash_start() couldn't get there (ESP_OK = it didn't fail, or wasn't called)
static char s_ssid[33];    // last Wi-Fi disconnect: network name and reason (0 = none since the last connect)
static uint8_t s_wifi_why;

static void wifi_watch(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        size_t n = MIN(d->ssid_len, sizeof s_ssid - 1);
        memcpy(s_ssid, d->ssid, n);
        s_ssid[n] = '\0';
        s_wifi_why = d->reason;
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_wifi_why = 0;
    }
}

// The usual ways a connection fails (WIFI_REASON_* in esp_wifi_types_generic.h); others are shown as a number.
static const char *wifi_reason_name(uint8_t r)
{
    switch (r) {
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 23:  return "802_1X_AUTH_FAILED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 210: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:  return NULL;
    }
}

static bool has_ipv4(esp_netif_t *netif, void *ctx)
{
    esp_netif_ip_info_t ip;
    return esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// On the network = airflash_start() succeeded and some interface (Wi-Fi, Ethernet, ...) has an IPv4 address.
static bool on_network(void)
{
    if (!s_started || esp_netif_get_nr_of_ifs() == 0) return false;  // no interface yet = lwIP may not even run
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    return esp_netif_find_if(has_ipv4, NULL) != NULL;  // walks the list inside the lwIP task: safe
#else
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        if (has_ipv4(n, NULL)) return true;
    }
    return false;
#endif
}

// Deadline passed: abort on purpose. The panic writes a core dump (with this message as its reason and the log tail),
// and a reset while the app is still pending verify makes the bootloader roll back to the updater.
static void __attribute__((noreturn)) give_up(void)
{
    static char msg[160];  // static: the panic handler prints it later, and the core dump keeps it
    char detail[96];
    const char *name = wifi_reason_name(s_wifi_why);
    if (s_start_err) snprintf(detail, sizeof detail, "airflash_start() failed (%s)", esp_err_to_name(s_start_err));
    else if (s_wifi_why) snprintf(detail, sizeof detail, "Wi-Fi '%s': reason %u%s%s", s_ssid, s_wifi_why, name ? " " : "", name ? name : "");
    else snprintf(detail, sizeof detail, "%s", s_started ? "no IP address" : "airflash_start() was not called");
    snprintf(msg, sizeof msg, "airflash: not on the network %d s after boot (%s)", CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S, detail);
    esp_system_abort(msg);
}

// First boot after a flash (the app is pending verify): confirm it once it's on the network, give up at the deadline.
// Works even if airflash_start() is never reached (e.g. the app's own Wi-Fi code retries a wrong password forever).
// A task, not a timer callback: confirming writes flash and the abort writes a core dump on this stack.
static void verify_task(void *arg)
{
    bool watching = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // The default event loop is the app's to create, never ours: keep trying until it exists.
        if (!watching && &WIFI_EVENT) {
            watching = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_watch, NULL) != ESP_ERR_INVALID_STATE;
        }
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK ||
            st != ESP_OTA_IMG_PENDING_VERIFY) {
            break;  // the app confirmed itself (or rolled back): nothing left to do
        }
        if (on_network()) {
#if CONFIG_AIRFLASH_AUTO_MARK_VALID
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "new app confirmed (valid)");
#else
            ESP_LOGW(TAG, "new app pending verify: call esp_ota_mark_app_valid_cancel_rollback() once healthy");
#endif
            break;
        }
        if (CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S && esp_timer_get_time() >= CONFIG_AIRFLASH_ROLLBACK_TIMEOUT_S * 1000000LL) {
            give_up();
        }
    }
    if (watching) esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_watch);
    vTaskDelete(NULL);
}

#if CONFIG_AIRFLASH_CRASH_LOOP_COUNT
static void young_over(void *arg)
{
    s_log.young = 0;  // survived the window: a crash from now on doesn't count as a crash loop
}
#endif

// A confirmed app that crashes on every boot before Wi-Fi is up can't be reached again. After
// AIRFLASH_CRASH_LOOP_COUNT boots in a row that crashed within AIRFLASH_CRASH_LOOP_WINDOW_S, mark it invalid: the
// bootloader then skips it and starts the updater (/info: "app_crash_loop"), ready for a fixed build.
static void crash_loop_guard(bool armed)
{
#if CONFIG_AIRFLASH_CRASH_LOOP_COUNT
    s_log.streak = s_kept && s_log.young && is_crash(esp_reset_reason()) ? s_log.streak + 1 : 0;
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = young_over, .name = "airflash"};
    // no timer = no way to tell young from old: count nothing this boot (fail safe)
    s_log.young = esp_timer_create(&args, &t) == ESP_OK &&
                  esp_timer_start_once(t, CONFIG_AIRFLASH_CRASH_LOOP_WINDOW_S * 1000000ULL) == ESP_OK;
    if (!armed || s_log.streak < CONFIG_AIRFLASH_CRASH_LOOP_COUNT) return;

    ESP_LOGE(TAG, "crashed %lu times in a row within %d s of boot: parking in the updater",
             (unsigned long)s_log.streak, CONFIG_AIRFLASH_CRASH_LOOP_WINDOW_S);
    s_log.streak = 0;
    // Writes state INVALID into this app's otadata entry and restarts; works for a VALID app too (no state check).
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGW(TAG, "can't park in the updater (%s): running the app anyway", esp_err_to_name(err));  // e.g. no updater
#endif
}

// After all constructors (esp_reset_reason() is set up in one, in no particular order) and before the scheduler,
// so before app_main(). Heap and flash work here.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
ESP_SYSTEM_INIT_FN(airflash_boot, SECONDARY, BIT(0), 250)
#else
ESP_SYSTEM_INIT_FN(airflash_boot, BIT(0), 250)  // IDF < 5.3: one stage, also after the constructors
#endif
{
    store_init();  // already done, unless "Capture logs from before app_main()" is off
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK) {
        return ESP_OK;  // not an OTA app (factory, no otadata): no rollback, nothing to guard
    }
    bool pending = st == ESP_OTA_IMG_PENDING_VERIFY;
    crash_loop_guard(!pending);  // pending: a crash already rolls back to the updater
    if (pending) xTaskCreate(verify_task, "airflash_verify", 4096, NULL, 5, NULL);
    return ESP_OK;  // always: anything else aborts the boot
}

/* ---------- optional built-in Wi-Fi (for apps that don't connect themselves) ---------- */

#if CONFIG_AIRFLASH_WIFI_ENABLE
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();  // retry forever: a board that gives up can't be reached
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

// One clear line for the step that failed; airflash_start() then leaves the app running without airflash.
#define WIFI_TRY(what, call) do {                                                                                  \
        esp_err_t e_ = (call);                                                                                     \
        if (e_ != ESP_OK) {                                                                                        \
            ESP_LOGE(TAG, "built-in Wi-Fi: %s failed (%s): the app runs without airflash", what, esp_err_to_name(e_)); \
            return e_;                                                                                             \
        }                                                                                                          \
    } while (0)

static esp_err_t wifi_start(void)
{
    if (!CONFIG_AIRFLASH_WIFI_SSID[0]) {
        ESP_LOGE(TAG, "built-in Wi-Fi is on but no SSID is set: idf.py menuconfig → airflash");
        return ESP_ERR_WIFI_SSID;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        WIFI_TRY("nvs_flash_erase", nvs_flash_erase());
        err = nvs_flash_init();
    }
    WIFI_TRY("nvs_flash_init", err);
    WIFI_TRY("esp_netif_init", esp_netif_init());
    err = esp_event_loop_create_default();
    WIFI_TRY("esp_event_loop_create_default", err == ESP_ERR_INVALID_STATE ? ESP_OK : err);  // the app made it: fine
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {  // a 2nd default station would assert
        ESP_LOGW(TAG, "built-in Wi-Fi is on, but the app set up Wi-Fi itself: using the app's "
                      "(turn it off: idf.py menuconfig → airflash)");
        return ESP_OK;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, s_name);  // DHCP hostname = router's client list; best effort (max 32 chars)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    WIFI_TRY("esp_wifi_init", esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_AIRFLASH_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_AIRFLASH_WIFI_PASSWORD, sizeof wc.sta.password);
    WIFI_TRY("esp_wifi_set_mode", esp_wifi_set_mode(WIFI_MODE_STA));
    WIFI_TRY("esp_wifi_set_config", esp_wifi_set_config(WIFI_IF_STA, &wc));
    WIFI_TRY("esp_wifi_start", esp_wifi_start());
    return ESP_OK;
}
#endif

/* ---------- public entry point ---------- */

// <name>.local and the _airflash._tcp service `airflash find` asks for (one multicast packet instead of knocking on
// every address). Best effort: without mDNS the board is still reachable by its IP address.
static void mdns_start(void)
{
    // espressif/mdns: mdns_init() returns ESP_OK when mDNS already runs, so ask first whether the app named the board
    char app_name[MDNS_NAME_BUF_LEN];
    bool app_named = mdns_hostname_get(app_name) == ESP_OK && app_name[0];
    esp_err_t err = mdns_init();  // INVALID_STATE (older mdns versions) = already running: reuse it
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS didn't start (%s): no %s.local, and `airflash find` won't list this board (use its IP "
                      "address). The rest of airflash keeps running", esp_err_to_name(err), s_name);
        return;
    }
    if (app_named) {
        strlcpy(s_name, app_name, sizeof s_name);
        ESP_LOGI(TAG, "keeping the app's mDNS name %s.local", s_name);
    } else {
        mdns_hostname_set(s_name);
    }
    mdns_service_add(NULL, "_airflash", "_tcp", CONFIG_AIRFLASH_HTTP_PORT, NULL, 0);
}

// Never the reason the app stops: whatever fails is logged (what, why, a hint) and skipped; always returns ESP_OK.
// Without its HTTP server airflash isn't reachable, so nothing else starts then: on the first boot after a flash the
// deadline rolls back to the updater (the board stays flashable), on later boots the app just runs without airflash.
esp_err_t airflash_start(void)
{
    if (s_started) return ESP_OK;  // called twice
    airflash_capture_logs();  // no-op if the early hook already ran
    report_boot();
    board_name();  // before wifi_start(): it's also the DHCP hostname
    load_token();  // before anything is served
    if (!s_key[0]) {
        ESP_LOGW(TAG, "no airflash token on this board: anyone on this network can flash it and read its logs "
                      "(reinstall the updater over USB to set one)");
    }

#if CONFIG_AIRFLASH_WIFI_ENABLE
    if ((s_start_err = wifi_start()) != ESP_OK) return ESP_OK;  // already logged; doesn't wait for the connection
#endif

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = CONFIG_AIRFLASH_HTTP_PORT;
    hcfg.ctrl_port = CONFIG_AIRFLASH_HTTP_CTRL_PORT;  // must differ from every httpd the app runs itself
    hcfg.max_open_sockets = 2;                        // control requests are rare; leave lwIP sockets to the app
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &hcfg);
    if (err != ESP_OK) {  // httpd_start() cleans up after itself
        s_start_err = err;
        ESP_LOGE(TAG, "control server didn't start (%s): http port %d or internal port %d already used by the app? "
                      "change CONFIG_AIRFLASH_HTTP_PORT / CONFIG_AIRFLASH_HTTP_CTRL_PORT. The app runs without airflash",
                 esp_err_to_name(err), CONFIG_AIRFLASH_HTTP_PORT, CONFIG_AIRFLASH_HTTP_CTRL_PORT);
        return ESP_OK;
    }
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/info", .method = HTTP_GET, .handler = info_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/nonce", .method = HTTP_GET, .handler = nonce_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/reboot-to-updater", .method = HTTP_POST, .handler = reboot_to_updater_post});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_GET, .handler = coredump_get});
    httpd_register_uri_handler(srv, &(httpd_uri_t){.uri = "/coredump", .method = HTTP_DELETE, .handler = coredump_delete});
    s_started = true;  // reachable: from now on an IP address counts as "on the network" (verify_task confirms)

    mdns_start();
    if (xTaskCreate(log_server_task, "airflash_log", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "log stream didn't start (%s): not enough free heap for its task. The app and the control "
                      "port keep running", esp_err_to_name(ESP_ERR_NO_MEM));
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "airflash %s in %s %s as %s.local: logs on tcp port %d, control on http port %d",
             AIRFLASH_VERSION, app->project_name, app->version, s_name, CONFIG_AIRFLASH_LOG_PORT, CONFIG_AIRFLASH_HTTP_PORT);
    return ESP_OK;
}
