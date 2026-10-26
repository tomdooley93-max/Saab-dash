// Wi-Fi: the dongle joins the phone's hotspot (station). With no hotspot stored, or none
// found within 30 s, it also opens its own network "Saab-Dash" so the app is still
// reachable on http://192.168.4.1 and the hotspot can be set from its console.
// The station settings are kept by the Wi-Fi driver in NVS.
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "dash.h"

#define AP_SSID "Saab-Dash"
#define AP_PASS "saab9-3t8"
#define HOSTNAME "saab-dash"

static const char *TAG = "net";
static esp_netif_t *sta_if;
static bool have_sta, ap_on, connected;
static TimerHandle_t fallback;

static void start_ap(void)
{
    if (ap_on) return;
    ap_on = true;
    wifi_config_t ap = { .ap = { .ssid = AP_SSID, .ssid_len = strlen(AP_SSID), .password = AP_PASS,
                                 .channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK } };
    esp_wifi_set_mode(have_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGI(TAG, "Eget Wi-Fi %s på, appen nås på http://192.168.4.1", AP_SSID);
}

static void fallback_cb(TimerHandle_t t)
{
    if (!connected) start_ap();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (have_sta) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (connected) xTimerReset(fallback, 0);
        connected = false;
        if (have_sta) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        connected = true;
        ESP_LOGI(TAG, "Ansluten till hotspot, IP " IPSTR, IP2STR(&e->ip_info.ip));
        if (ap_on) {
            ap_on = false;
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_if = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_if, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    have_sta = sta.sta.ssid[0] != 0;
    fallback = xTimerCreate("ap", pdMS_TO_TICKS(30000), pdFALSE, NULL, fallback_cb);
    if (have_sta) xTimerStart(fallback, 0);
    else start_ap();

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);      // 20 packets/s; power saving only adds latency

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("Saab Dash");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

bool net_set_wifi(const char *ssid, const char *pass)
{
    size_t ls = strlen(ssid), lp = strlen(pass);
    if (!ls || ls > 32 || lp > 63 || (lp && lp < 8)) return false;
    wifi_config_t c = { 0 };
    memcpy(c.sta.ssid, ssid, ls);
    memcpy(c.sta.password, pass, lp);
    if (!have_sta) {
        have_sta = true;
        esp_wifi_set_mode(ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &c) != ESP_OK) return false;
    esp_wifi_disconnect();
    esp_wifi_connect();
    xTimerReset(fallback, 0);
    return true;
}

void net_info(char *ip, size_t n, int *rssi)
{
    esp_netif_ip_info_t i;
    wifi_ap_record_t ap;
    *rssi = 0;
    if (connected && esp_netif_get_ip_info(sta_if, &i) == ESP_OK) {
        snprintf(ip, n, IPSTR, IP2STR(&i.ip));
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) *rssi = ap.rssi;
    } else {
        snprintf(ip, n, "192.168.4.1");
    }
}
