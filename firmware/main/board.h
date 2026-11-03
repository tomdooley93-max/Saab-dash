// WiCAN Pro (ESP32-S3) pins, from hw_config.h and main.c in the WiCAN firmware (branch wican-pro).
#pragma once
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

// The ESP32's own CAN controller, on the P-bus (OBD pins 6/14)
#define CAN_TX GPIO_NUM_2
#define CAN_RX GPIO_NUM_1
#define CAN_STDBY GPIO_NUM_38       // high = transceiver in standby

// The OBD chip (ELM327/STN-style commands over UART). It reaches every OBD protocol,
// among them single-wire CAN on pin 1, which is the I-bus.
#define OBD_UART_TX GPIO_NUM_16
#define OBD_UART_RX GPIO_NUM_15
#define OBD_RESET GPIO_NUM_41       // open drain, low = reset
#define OBD_SLEEP GPIO_NUM_9        // high = awake
#define OBD_LED_EN GPIO_NUM_42

// Battery: divider 1:11 from hardware 1.5 (1.40 boards: 7.25 and no offset)
#define VBAT_CH ADC_CHANNEL_3
#define VBAT_SCALE 11.0f
#define VBAT_OFFSET 0.1f
