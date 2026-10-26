// Values from the car over the P-bus (500 kbit/s, OBD pins 6/14), decoded as in the old
// Pico gauge: rpm from the broadcast 0x110, the rest by OBD mode 01 requests, fault codes
// by mode 03/07, and air mass, AFR, ethanol and adaption from T8 symbols over KWP2000
// while the app shows the air mass page.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "dash.h"

// WiCAN-OBD pins (hw_config.h in the WiCAN firmware)
#define CAN_TX GPIO_NUM_0
#define CAN_RX GPIO_NUM_3
#define CAN_STDBY GPIO_NUM_6        // high = transceiver in standby

#define ID_RPM 0x110
#define ID_OBD 0x7DF                // functional request, every ECU
#define ID_ECU 0x7E0                // engine (T8), physical
#define ID_ECU_REPLY 0x7E8
#define SLIDE_AIRMASS 5
#define MAX_DTC 16

static const char *TAG = "car";

static struct {
    uint8_t flags;
    int16_t boost;          // mbar relative, 0x7FFF unknown
    uint16_t rpm, airm_req, airm_act, afr;
    uint8_t speed, coolant, iat, batt, ethanol, fuel;
} car;
struct dtc { uint16_t code; uint8_t status; };
static struct dtc dtcs[MAX_DTC];
static uint8_t n_dtc;
static portMUX_TYPE dtc_lock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t replies;       // frames from 0x7E8
static volatile int64_t last_frame_us = -1000000000;
static volatile uint8_t slide;
static volatile bool want_clear, want_read;

static void forget_values(void)
{
    car.flags = 0;
    car.boost = 0x7FFF;
    car.rpm = car.airm_req = car.airm_act = car.afr = 0xFFFF;
    car.speed = car.coolant = car.iat = car.batt = car.ethanol = car.fuel = 0xFF;
}

int car_idle_ms(void) { return (esp_timer_get_time() - last_frame_us) / 1000; }

void car_sleep(void)
{
    twai_stop();
    gpio_set_level(CAN_STDBY, 1);
}

/* ---------- CAN and ISO-TP ---------- */

static bool send(uint32_t id, const uint8_t f[8])
{
    twai_message_t m = { .identifier = id, .data_length_code = 8 };
    memcpy(m.data, f, 8);
    return twai_transmit(&m, pdMS_TO_TICKS(20)) == ESP_OK;
}

static void rx_task(void *arg)
{
    twai_message_t m;
    for (;;) {
        if (twai_receive(&m, pdMS_TO_TICKS(1000)) != ESP_OK) continue;
        last_frame_us = esp_timer_get_time();
        if (m.extd || m.rtr) continue;
        if (m.identifier == ID_RPM && m.data_length_code >= 3)
            car.rpm = ((m.data[1] << 8) | m.data[2]) / 4;
        else if (m.identifier == ID_ECU_REPLY)
            xQueueSend(replies, &m, 0);
    }
}

static void copy_out(uint8_t *out, size_t max, size_t pos, const uint8_t *src, size_t n)
{
    if (pos >= max) return;
    memcpy(out + pos, src, n < max - pos ? n : max - pos);
}

// Sends a request (multi-frame if longer than 7 bytes) and returns the length of the
// engine's reply, 0 on timeout. "Response pending" (7F xx 78) keeps waiting.
static size_t request(uint32_t id, const uint8_t *req, size_t len, uint8_t *out, size_t max, int timeout_ms)
{
    uint8_t f[8] = { 0 };
    twai_message_t m;
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    xQueueReset(replies);
    if (len <= 7) {
        f[0] = len;
        memcpy(f + 1, req, len);
        if (!send(id, f)) return 0;
    } else {
        f[0] = 0x10 | (len >> 8);
        f[1] = len;
        memcpy(f + 2, req, 6);
        if (!send(id, f)) return 0;
        if (xQueueReceive(replies, &m, wait) != pdTRUE || (m.data[0] & 0xF0) != 0x30) return 0;
        uint8_t st = m.data[2] <= 0x7F ? m.data[2] : 1;      // separation time, ms
        uint8_t sn = 1;
        for (size_t off = 6; off < len; off += 7) {
            size_t n = len - off < 7 ? len - off : 7;
            memset(f, 0, sizeof f);
            f[0] = 0x20 | (sn++ & 0x0F);
            memcpy(f + 1, req + off, n);
            vTaskDelay(pdMS_TO_TICKS(st ? st : 1));
            if (!send(id, f)) return 0;
        }
    }
    for (;;) {
        if (xQueueReceive(replies, &m, wait) != pdTRUE) return 0;
        uint8_t pci = m.data[0] >> 4;
        if (pci == 0) {
            size_t n = m.data[0] & 0x0F;
            if (n > 7) n = 7;
            if (n >= 3 && m.data[1] == 0x7F && m.data[3] == 0x78) continue;
            copy_out(out, max, 0, m.data + 1, n);
            return n < max ? n : max;
        }
        if (pci == 1) {
            size_t total = ((m.data[0] & 0x0F) << 8) | m.data[1], got = 6;
            copy_out(out, max, 0, m.data + 2, 6);
            uint8_t fc[8] = { 0x30, 0, 0 };
            send(ID_ECU, fc);
            while (got < total) {
                if (xQueueReceive(replies, &m, wait) != pdTRUE) return 0;
                if ((m.data[0] >> 4) != 2) continue;
                size_t n = total - got < 7 ? total - got : 7;
                copy_out(out, max, got, m.data + 1, n);
                got += n;
            }
            return total < max ? total : max;
        }
    }
}

/* ---------- OBD mode 01 ---------- */

static int pid(uint8_t p, uint8_t *a)
{
    uint8_t q[2] = { 0x01, p }, r[8];
    size_t n = request(ID_OBD, q, sizeof q, r, sizeof r, 100);
    if (n < 3 || r[0] != 0x41 || r[1] != p) return 0;
    memcpy(a, r + 2, n - 2);
    return n - 2;
}

// boost gets 5 of every 8 requests, as in the old gauge
static void poll_obd(unsigned step)
{
    static const uint8_t slow[] = { 0x0F, 0x2F, 0x52 };     // IAT, fuel, ethanol
    uint8_t a[6];
    switch (step % 8) {
    case 1:
        if (pid(0x0D, a)) car.speed = a[0] < 0xFF ? a[0] : 0xFE;
        break;
    case 3:
        if (pid(0x42, a) >= 2) car.batt = ((a[0] << 8) | a[1]) / 100;
        break;
    case 5:
        if (pid(0x05, a)) car.coolant = a[0];               // A - 40 °C, same coding as the packet
        break;
    case 7: {
        uint8_t p = slow[(step / 8) % sizeof slow];
        if (!pid(p, a)) break;
        if (p == 0x0F) car.iat = a[0];
        else if (p == 0x2F) car.fuel = a[0] * 100 / 255;
        else car.ethanol = a[0] * 100 / 255;
        break;
    }
    default:
        if (pid(0x0B, a)) car.boost = a[0] * 10 - 1000;     // MAP kPa absolute → mbar relative
        break;
    }
}

/* ---------- fault codes ---------- */

static void send_dtcs(int fd)
{
    uint8_t t[2 + MAX_DTC * 3];
    web_send(fd, MSG_DTC, t, car_dtc(t, sizeof t));
}

static void read_dtcs(void)
{
    struct dtc found[MAX_DTC];
    uint8_t n = 0, r[64];
    for (int i = 0; i < 2; i++) {
        uint8_t mode = i ? 0x07 : 0x03, status = i ? 0x04 : 0x08;     // pending : confirmed
        size_t len = request(ID_OBD, &mode, 1, r, sizeof r, 300);
        if (len < 1 || r[0] != mode + 0x40) {
            if (!i) return;         // keep the last list
            continue;
        }
        const uint8_t *p = r + 1;
        size_t count = (len - 1) / 2;
        if (len >= 2 && r[1] * 2u == len - 2) { p = r + 2; count = r[1]; }  // with a count byte
        for (size_t k = 0; k < count; k++) {
            uint16_t code = (p[2 * k] << 8) | p[2 * k + 1];
            if (!code) continue;
            uint8_t j = 0;
            while (j < n && found[j].code != code) j++;
            if (j < n) found[j].status |= status;
            else if (n < MAX_DTC) found[n++] = (struct dtc){ code, status };
        }
    }
    bool changed;
    portENTER_CRITICAL(&dtc_lock);
    changed = n != n_dtc || memcmp(found, dtcs, n * sizeof found[0]);
    memcpy(dtcs, found, n * sizeof found[0]);
    n_dtc = n;
    portEXIT_CRITICAL(&dtc_lock);
    if (changed) send_dtcs(-1);
}

static void clear_dtcs(void)
{
    uint8_t q = 0x04, r[8];
    size_t n = request(ID_OBD, &q, 1, r, sizeof r, 1000);
    dash_log(n && r[0] == 0x44 ? "Felkoder raderade" : "Motorn svarade inte på radering av felkoder");
    read_dtcs();
}

size_t car_dtc(uint8_t *out, size_t max)
{
    if (max < 2) return 0;
    portENTER_CRITICAL(&dtc_lock);
    uint8_t n = n_dtc;
    if (2 + n * 3u > max) n = (max - 2) / 3;
    out[0] = 1;
    out[1] = n;
    for (uint8_t i = 0; i < n; i++) {
        out[2 + 3 * i] = dtcs[i].code >> 8;     // big-endian, SAE format
        out[3 + 3 * i] = dtcs[i].code;
        out[4 + 3 * i] = dtcs[i].status;
    }
    portEXIT_CRITICAL(&dtc_lock);
    return 2 + n * 3;
}

/* ---------- T8 symbols over KWP2000 ---------- */

static const struct { uint32_t addr; uint8_t len; } SYMBOLS[] = {
    { 0x1027B6, 2 },    // AirMassMast_m_Request, mg/c
    { 0x1066E8, 2 },    // MAF_m_AirInlet, mg/c
    { 0x106C30, 4 },    // FMast_m_FuelActual
    { 0x1048E8, 2 },    // FFFuelAdap_X_blend, ethanol %
    { 0x105DD8, 1 },    // FuelAdap_ST_State
    { 0x105E04, 1 },    // FuelAdapProt_ST_Stable
};

static bool kwp(const uint8_t *q, size_t len, uint8_t *r, size_t max, size_t *got)
{
    size_t n = request(ID_ECU, q, len, r, max, 250);
    if (got) *got = n;
    return n && r[0] == q[0] + 0x40;
}

// seed to key, from TrionicCANLib
static uint16_t t8_key(uint16_t seed, uint8_t level)
{
    uint16_t k = (seed >> 5) | (seed << 11);
    k += 0xB988;
    if (level == 0xFB) {
        k ^= 0x8749; k += 0x06D3; k ^= 0xCFDF;
    } else if (level == 0xFD) {
        k /= 3; k ^= 0x8749; k += 0x0ACF; k ^= 0x81BF;
    }
    return k;
}

static bool t8_unlock(void)
{
    static const uint8_t levels[] = { 0xFD, 0xFB, 0x01 };
    uint8_t r[8];
    size_t n;
    for (size_t i = 0; i < sizeof levels; i++) {
        uint8_t q[4] = { 0x27, levels[i] };
        if (!kwp(q, 2, r, sizeof r, &n) || n < 4) continue;
        uint16_t seed = (r[2] << 8) | r[3];
        if (!seed) return true;     // already unlocked
        uint16_t key = t8_key(seed, levels[i]);
        q[1] = levels[i] + 1; q[2] = key >> 8; q[3] = key;
        if (kwp(q, 4, r, sizeof r, NULL)) return true;
    }
    return false;
}

static bool t8_setup(void)
{
    uint8_t r[8], q[3 + 6 * 9] = { 0x3B, 0x17, 0xF0 };
    static const uint8_t session[] = { 0x10, 0x02 }, reset[] = { 0x3B, 0x17, 0xF0, 0x04 };
    if (!kwp(session, sizeof session, r, sizeof r, NULL)) return false;
    if (!t8_unlock()) { dash_log("T8: upplåsningen nekades"); return false; }
    if (!kwp(reset, sizeof reset, r, sizeof r, NULL)) return false;
    size_t len = 3;
    for (size_t i = 0; i < sizeof SYMBOLS / sizeof SYMBOLS[0]; i++) {
        uint8_t *e = q + len;
        e[0] = 0x03; e[1] = 0x00; e[2] = SYMBOLS[i].len;
        e[3] = SYMBOLS[i].addr >> 16; e[4] = SYMBOLS[i].addr >> 8; e[5] = SYMBOLS[i].addr;
        len += 6;
    }
    if (!kwp(q, len, r, sizeof r, NULL)) return false;
    dash_log("T8: diagnossession igång");
    return true;
}

static bool t8_read(void)
{
    static const uint8_t tester[] = { 0x3E }, read[] = { 0x1A, 0x18, 0xF0 };
    uint8_t r[32];
    size_t n;
    request(ID_ECU, tester, sizeof tester, r, sizeof r, 50);
    if (!kwp(read, sizeof read, r, sizeof r, &n) || n < 2 + 12) return false;
    const uint8_t *d = r + 2;
    uint16_t req = (d[0] << 8) | d[1], act = (d[2] << 8) | d[3];
    uint32_t fuel = ((uint32_t)d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
    uint16_t blend = (d[8] << 8) | d[9];
    car.airm_req = req;
    car.airm_act = act;
    car.afr = fuel ? (uint16_t)((uint64_t)act * 10000 / fuel) : 0xFFFF;    // AFR × 100
    car.ethanol = blend <= 100 ? blend : 0xFF;
    if (d[10] && !d[11]) car.flags |= 2; else car.flags &= ~2;
    return true;
}

/* ---------- loops ---------- */

static void poll_task(void *arg)
{
    bool diag = false;
    int fails = 0;
    int64_t last_dtc = 0, next_setup = 0;
    unsigned step = 0;
    for (;;) {
        twai_status_info_t s;
        if (twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
        if (twai_get_status_info(&s) == ESP_OK && s.state == TWAI_STATE_STOPPED) twai_start();
        // never talk on a sleeping bus: wait for the car's own traffic
        if (car_idle_ms() > 500) {
            diag = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (want_clear) { want_clear = false; clear_dtcs(); last_dtc = now; }
        if (want_read || now - last_dtc > 10000000) { want_read = false; read_dtcs(); last_dtc = now; }

        if (slide == SLIDE_AIRMASS && (diag || now > next_setup)) {
            if (!diag && !(diag = t8_setup())) next_setup = now + 5000000;
            if (diag && t8_read()) fails = 0;
            else if (diag && ++fails > 3) { diag = false; dash_log("T8: tappade diagnossessionen"); }
            if (diag) {
                car.flags |= 8;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }
        diag = false;
        car.flags &= ~8;
        poll_obd(step++);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void data_task(void *arg)
{
    uint8_t d[20], seq = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int idle = car_idle_ms();
        if (idle > 2000) forget_values();
        uint8_t n;
        portENTER_CRITICAL(&dtc_lock);
        n = n_dtc;
        bool cel = false;
        for (uint8_t i = 0; i < n_dtc; i++) cel |= dtcs[i].status & 0x08;
        portEXIT_CRITICAL(&dtc_lock);
        d[0] = 1;
        d[1] = (car.flags & ~5) | (idle < 500 ? 1 : 0) | (cel ? 4 : 0);
        put16(d + 2, (uint16_t)car.boost);
        put16(d + 4, car.rpm);
        d[6] = car.speed; d[7] = car.coolant; d[8] = car.iat; d[9] = car.batt;
        put16(d + 10, car.airm_req);
        put16(d + 12, car.airm_act);
        put16(d + 14, car.afr);
        d[16] = car.ethanol; d[17] = car.fuel;
        d[18] = n;
        d[19] = seq++;
        web_send(-1, MSG_DATA, d, sizeof d);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
    }
}

void car_start(void)
{
    forget_values();
    gpio_reset_pin(CAN_STDBY);
    gpio_set_direction(CAN_STDBY, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_STDBY, 0);
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
    g.rx_queue_len = 64;            // the P-bus is busy; dropped frames could be ECU replies
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
    replies = xQueueCreate(8, sizeof(twai_message_t));
    xTaskCreate(rx_task, "can_rx", 3072, NULL, 7, NULL);
    xTaskCreate(poll_task, "can_poll", 4096, NULL, 6, NULL);
    xTaskCreate(data_task, "data", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "CAN 500 kbit/s igång");
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void console(const char *line)
{
    const esp_app_desc_t *d = esp_app_get_description();
    if (!strcmp(line, "v")) {
        dash_log("fw %s, ESP-IDF %s, byggd %s %s", d->version, d->idf_ver, d->date, d->time);
    } else if (!strcmp(line, "can")) {
        twai_status_info_t s = { 0 };
        twai_get_status_info(&s);
        dash_log("CAN: senaste ram för %d ms sedan, tillstånd %d, rx-fel %" PRIu32 ", tx-fel %" PRIu32 ", buss-fel %" PRIu32,
                 car_idle_ms(), s.state, s.rx_error_counter, s.tx_error_counter, s.bus_error_count);
    } else if (!strncmp(line, "wifi ", 5)) {
        // wifi SSID|LÖSENORD (an iPhone's hotspot name usually has spaces, hence the bar)
        char buf[100];
        snprintf(buf, sizeof buf, "%s", line + 5);
        char *bar = strchr(buf, '|');
        if (bar) *bar = 0;
        if (net_set_wifi(buf, bar ? bar + 1 : ""))
            dash_log("Wi-Fi sparat: %s, ansluter", buf);
        else
            dash_log("Skriv: wifi SSID|LÖSENORD");
    } else if (!strcmp(line, "reboot")) {
        dash_log("Startar om");
        xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    } else {
        dash_log("Okänt kommando. Finns: v, can, wifi SSID|LÖSENORD, reboot");
    }
}

void car_command(int fd, const uint8_t *b, size_t len)
{
    if (!len) return;
    switch (b[0]) {
    case 0x01:
        want_clear = true;
        break;
    case 0x02:
        want_read = true;
        send_dtcs(fd);
        break;
    case 0x10:
        if (len > 1) slide = b[1];
        break;
    case 0x30: {
        char line[100];
        size_t n = len - 1 < sizeof line - 1 ? len - 1 : sizeof line - 1;
        memcpy(line, b + 1, n);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
        line[n] = 0;
        console(line);
        break;
    }
    }
}
