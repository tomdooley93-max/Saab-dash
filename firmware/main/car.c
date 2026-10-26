// Values from the car. The CAN part (TWAI on the P-bus, KWP diagnostics for air mass)
// is not written yet: it moves over from the old gauge code once that is in the repo.
// Until then every value is sent as unknown and can_alive is 0.
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dash.h"

static struct {
    uint8_t flags;
    int16_t boost;          // mbar relative, 0x7FFF unknown
    uint16_t rpm, airm_req, airm_act, afr;
    uint8_t speed, coolant, iat, batt, ethanol, fuel;
} car = { .boost = 0x7FFF, .rpm = 0xFFFF, .airm_req = 0xFFFF, .airm_act = 0xFFFF, .afr = 0xFFFF,
          .speed = 0xFF, .coolant = 0xFF, .iat = 0xFF, .batt = 0xFF, .ethanol = 0xFF, .fuel = 0xFF };
static volatile uint8_t slide;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void data_task(void *arg)
{
    uint8_t d[20], seq = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        d[0] = 1;
        d[1] = car.flags;
        put16(d + 2, (uint16_t)car.boost);
        put16(d + 4, car.rpm);
        d[6] = car.speed; d[7] = car.coolant; d[8] = car.iat; d[9] = car.batt;
        put16(d + 10, car.airm_req);
        put16(d + 12, car.airm_act);
        put16(d + 14, car.afr);
        d[16] = car.ethanol; d[17] = car.fuel;
        d[18] = 0;          // dtc_count
        d[19] = seq++;
        web_send(-1, MSG_DATA, d, sizeof d);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
    }
}

void car_start(void)
{
    xTaskCreate(data_task, "data", 3072, NULL, 5, NULL);
}

size_t car_dtc(uint8_t *out, size_t n)
{
    if (n < 2) return 0;
    out[0] = 1;     // version
    out[1] = 0;     // count
    return 2;
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
        dash_log("Okänt kommando. Finns: v, wifi SSID|LÖSENORD, reboot");
    }
}

void car_command(int fd, const uint8_t *b, size_t len)
{
    if (!len) return;
    switch (b[0]) {
    case 0x01:
        dash_log("Felkoder kan inte raderas än: CAN är inte inkopplat i firmwaren");
        break;
    case 0x02: {
        uint8_t t[2];
        web_send(fd, MSG_DTC, t, car_dtc(t, sizeof t));
        break;
    }
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
