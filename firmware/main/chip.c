// The WiCAN Pro's OBD chip, reached over UART with ELM327/STN-style text commands.
// The P-bus is read by the ESP32's own CAN controller (car.c); the chip is here for what
// that cannot reach, first of all the I-bus (single-wire CAN 33.3 kbit/s on OBD pin 1).
// For now the console passes commands straight through ("obd ATI"), so the I-bus can be
// explored in the car before any function is built on it.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "board.h"
#include "dash.h"

#define UART UART_NUM_1
#define FAST_BAUD 2000000           // what the WiCAN firmware leaves the chip at
#define SLOW_BAUD 115200            // the chip's default
#define LINE 100

static const char *TAG = "chip";
static QueueHandle_t cmds;
static bool ready;

// Send one command and collect the reply up to the '>' prompt.
static int ask(const char *cmd, char *out, size_t n, int timeout_ms)
{
    uart_flush_input(UART);
    uart_write_bytes(UART, cmd, strlen(cmd));
    uart_write_bytes(UART, "\r", 1);
    size_t len = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (len < n - 1 && xTaskGetTickCount() < end) {
        int r = uart_read_bytes(UART, (uint8_t *)out + len, 1, pdMS_TO_TICKS(20));
        if (r <= 0) continue;
        if (out[len] == '>') break;
        len++;
    }
    out[len] = 0;
    for (char *c = out; *c; c++)
        if (*c == '\r') *c = ' ';
    return len;
}

static bool find_baud(void)
{
    char r[64];
    int bauds[] = { FAST_BAUD, SLOW_BAUD };
    for (int i = 0; i < 2; i++) {
        uart_set_baudrate(UART, bauds[i]);
        if (ask("ATI", r, sizeof r, 500) > 0 && strstr(r, "ELM")) {
            ESP_LOGI(TAG, "%d baud: %s", bauds[i], r);
            return true;
        }
    }
    return false;
}

static void chip_task(void *arg)
{
    // Pulse reset and give the chip time to boot
    gpio_set_level(OBD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(OBD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    ready = find_baud();
    if (!ready) dash_log("OBD-kretsen svarar inte");

    char line[LINE], reply[256];
    for (;;) {
        xQueueReceive(cmds, line, portMAX_DELAY);
        if (!ready && !(ready = find_baud())) {
            dash_log("OBD-kretsen svarar inte");
            continue;
        }
        // ATMA and STMA stream until interrupted; give them a few seconds of traffic
        bool monitor = !strncasecmp(line, "ATMA", 4) || !strncasecmp(line, "STM", 3);
        if (ask(line, reply, sizeof reply, monitor ? 3000 : 2000) == 0) {
            dash_log("%s: inget svar", line);
            continue;
        }
        if (monitor) {
            uart_write_bytes(UART, "\r", 1);    // any character stops monitoring
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        dash_log("%s: %s", line, reply);
    }
}

void chip_command(const char *cmd)
{
    char line[LINE];
    snprintf(line, sizeof line, "%s", cmd);
    if (!cmds || xQueueSend(cmds, line, 0) != pdTRUE) dash_log("OBD-kretsen är upptagen");
}

void chip_sleep(void)
{
    gpio_set_level(OBD_SLEEP, 0);
}

void chip_start(void)
{
    gpio_reset_pin(OBD_LED_EN);
    gpio_set_direction(OBD_LED_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(OBD_LED_EN, 0);
    gpio_reset_pin(OBD_SLEEP);
    gpio_set_direction(OBD_SLEEP, GPIO_MODE_OUTPUT);
    gpio_set_level(OBD_SLEEP, 1);
    gpio_reset_pin(OBD_RESET);
    gpio_set_direction(OBD_RESET, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(OBD_RESET, 1);

    uart_config_t c = { .baud_rate = FAST_BAUD, .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART, &c));
    ESP_ERROR_CHECK(uart_set_pin(UART, OBD_UART_TX, OBD_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    cmds = xQueueCreate(2, LINE);
    xTaskCreate(chip_task, "chip", 4096, NULL, 4, NULL);
}
