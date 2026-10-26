// Shared between the firmware's parts. The wire format is in PROTOCOL.md.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MSG_DATA = 0x01, MSG_DTC = 0x02, MSG_LOG = 0x03 };

// net.c: Wi-Fi client to the phone's hotspot, own access point as fallback, mDNS
void net_start(void);
bool net_set_wifi(const char *ssid, const char *pass);
void net_info(char *ip, size_t n, int *rssi);

// web.c: app files, /api/*, WebSocket /ws
void web_start(void);
void web_send(int fd, uint8_t type, const void *data, size_t len);   // fd < 0: every app
void dash_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// car.c: values from the car and commands from the app
void car_start(void);
void car_command(int fd, const uint8_t *b, size_t len);
size_t car_dtc(uint8_t *out, size_t n);
int car_idle_ms(void);          // since the last CAN frame
void car_sleep(void);           // CAN off, transceiver to standby

// power.c: battery voltage and sleep while the car is off
void power_start(void);
float power_vbat(void);         // volts, < 0 unknown
