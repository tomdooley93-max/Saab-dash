// Sleep while the car is off. The OBD port has power all the time and Wi-Fi alone would
// drain the battery in days. Same rule as WiCAN: battery below 13.1 V (not charging) and
// a silent bus for a minute → CAN transceiver to standby, Wi-Fi off, light sleep with a
// voltage check every 2 s; above 13.1 V again, or traffic on CAN RX, → restart.
#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dash.h"

#define VBAT_CH ADC_CHANNEL_4
#define CAN_RX GPIO_NUM_3
#define AWAKE_V 13.1f
#define QUIET_MS 60000

static const char *TAG = "power";
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static volatile float vbat = -1;

float power_vbat(void) { return vbat; }

// WiCAN OBD: 100k/16k divider, plus 0.2 V for its input diode
static float read_vbat(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(adc, VBAT_CH, &raw) != ESP_OK) continue;
        if (!cali || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK) mv = raw * 3300 / 4095;
        sum += mv;
        n++;
    }
    return n ? (float)sum / n * 116.0f / 16000.0f + 0.2f : -1;
}

static void sleep_until_car_wakes(void)
{
    dash_log("Bilen är avstängd (%.1f V), dongeln sover", vbat);
    vTaskDelay(pdMS_TO_TICKS(300));
    car_sleep();
    esp_wifi_stop();
    // wake on CAN traffic too, if RX idles high with the transceiver in standby
    bool rx_wake = gpio_get_level(CAN_RX) == 1;
    if (rx_wake) {
        gpio_wakeup_enable(CAN_RX, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }
    for (;;) {
        esp_sleep_enable_timer_wakeup(2000000);
        esp_light_sleep_start();
        if (rx_wake && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) esp_restart();
        float v = read_vbat();
        if (v > AWAKE_V) esp_restart();
    }
}

static void power_task(void *arg)
{
    int64_t low_since = 0;
    for (;;) {
        float v = read_vbat();
        vbat = v;
        int64_t now = esp_timer_get_time();
        if (v > 6 && v < AWAKE_V) {       // below 6 V: on USB power, not in a car
            if (!low_since) low_since = now;
            if (now - low_since > QUIET_MS * 1000LL && car_idle_ms() > QUIET_MS) sleep_until_car_wakes();
        } else {
            low_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void power_start(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adc));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, VBAT_CH, &c));
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .chan = VBAT_CH,
                                           .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) cali = NULL;
    xTaskCreate(power_task, "power", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Batteri %.1f V", read_vbat());
}
