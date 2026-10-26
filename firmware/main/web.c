// HTTP and WebSocket, see PROTOCOL.md. The app and its icons live in the SPIFFS
// partition mounted at /www.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dash.h"

#define MAX_SOCKETS 7
#define APP "/www/index.html"
#define APP_NEW "/www/index.new"

static const char *TAG = "web";
static httpd_handle_t server;
static char build[24];      // the app's build stamp, read from index.html on demand

// Shown when the storage holds no app yet.
static const char SETUP_PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>Saab Dash</title><body style='font:17px system-ui;background:#000;color:#eee;padding:20px'>"
    "<h2>Saab Dash</h2><p>Ingen app i dongeln. Välj index.html:</p><input type=file id=f accept='.html'>"
    "<p id=m></p><script>f.onchange=async()=>{m.textContent='Skickar…';"
    "const r=await fetch('/api/app',{method:'POST',body:await f.files[0].arrayBuffer()});"
    "if(r.ok)location.reload();else m.textContent='Misslyckades: '+r.status}</script>";

/* ---------- sending to the apps ---------- */

struct out { int fd; size_t len; uint8_t buf[]; };

static void send_one(int fd, struct out *o)
{
    httpd_ws_frame_t f = { .final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = o->buf, .len = o->len };
    httpd_ws_send_frame_async(server, fd, &f);
}

// runs in the server task, so frames to one socket never interleave
static void send_work(void *arg)
{
    struct out *o = arg;
    if (o->fd >= 0) {
        send_one(o->fd, o);
    } else {
        int fds[MAX_SOCKETS];
        size_t n = MAX_SOCKETS;
        if (httpd_get_client_list(server, &n, fds) == ESP_OK)
            for (size_t i = 0; i < n; i++)
                if (httpd_ws_get_fd_info(server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) send_one(fds[i], o);
    }
    free(o);
}

void web_send(int fd, uint8_t type, const void *data, size_t len)
{
    if (!server) return;
    struct out *o = malloc(sizeof *o + len + 1);
    if (!o) return;
    o->fd = fd;
    o->len = len + 1;
    o->buf[0] = type;
    memcpy(o->buf + 1, data, len);
    if (httpd_queue_work(server, send_work, o) != ESP_OK) free(o);
}

void dash_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    ESP_LOGI("log", "%s", line);
    web_send(-1, MSG_LOG, line, n);
}

/* ---------- handlers ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static const char *app_build(void)
{
    if (build[0]) return build;
    snprintf(build, sizeof build, "?");
    FILE *f = fopen(APP, "r");
    if (!f) return build;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "const BUILD = '");
        if (p) {
            p += 15;
            size_t n = strcspn(p, "'");
            if (n >= sizeof build) n = sizeof build - 1;
            memcpy(build, p, n);
            build[n] = 0;
            break;
        }
    }
    fclose(f);
    return build;
}

static esp_err_t info_get(httpd_req_t *req)
{
    char ip[16], json[256];
    int rssi;
    net_info(ip, sizeof ip, &rssi);
    // vbat is null until the dongle's voltage divider is read
    snprintf(json, sizeof json,
             "{\"device\":\"saab-dash\",\"fw\":\"%s\",\"app\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime\":%lld,\"vbat\":null}",
             esp_app_get_description()->version, app_build(), ip, rssi, esp_timer_get_time() / 1000000);
    return send_json(req, "200 OK", json);
}

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

static esp_err_t file_get(httpd_req_t *req)
{
    char path[64];
    const char *uri = req->uri;
    size_t n = strcspn(uri, "?#");
    if (n == 1) { uri = "/index.html"; n = 11; }
    if (n + 5 > sizeof path || strstr(uri, "..")) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    snprintf(path, sizeof path, "/www%.*s", (int)n, uri);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!strcmp(path, APP)) {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            return httpd_resp_send(req, SETUP_PAGE, sizeof SETUP_PAGE - 1);
        }
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[1024];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// reads the next part of the body, retrying on timeouts; <= 0 means the upload broke off
static int recv_part(httpd_req_t *req, char *buf, size_t left, size_t size)
{
    for (int tries = 0; tries < 5; tries++) {
        int n = httpd_req_recv(req, buf, left < size ? left : size);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
    }
    return -1;
}

static bool looks_like_html(const char *b, int n)
{
    for (int i = 0; i + 5 <= n; i++)
        if (!strncasecmp(b + i, "<html", 5)) return true;
    return false;
}

// new app: written to a temporary file, swapped in only once it has all arrived
static esp_err_t app_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 256 * 1024)
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"fel storlek\"}");
    FILE *f = fopen(APP_NEW, "wb");
    if (!f) return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"lagringen\"}");
    char buf[1024];
    size_t left = req->content_len;
    bool first = true, ok = true;
    while (left && ok) {
        int n = recv_part(req, buf, left, sizeof buf);
        if (n <= 0) { ok = false; break; }
        if (first && !looks_like_html(buf, n)) {
            fclose(f);
            unlink(APP_NEW);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"inte en html-fil\"}");
        }
        first = false;
        ok = fwrite(buf, 1, n, f) == (size_t)n;
        left -= n;
    }
    fclose(f);
    if (!ok) {
        unlink(APP_NEW);
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"avbruten\"}");
    }
    // SPIFFS cannot rename onto an existing file, so the old app goes just before;
    // if the rename still fails, web_start finishes it on the next boot
    unlink(APP);
    if (rename(APP_NEW, APP) != 0)
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"kunde inte spara\"}");
    build[0] = 0;
    dash_log("Ny app sparad (%u byte, build %s)", (unsigned)req->content_len, app_build());
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// new firmware: written to the free OTA slot; the bootloader falls back if it does not start
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || req->content_len < 1024 || req->content_len > slot->size)
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"fel storlek\"}");
    esp_ota_handle_t h;
    if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"ota_begin\"}");
    dash_log("Tar emot firmware (%u byte)", (unsigned)req->content_len);
    char buf[1024];
    size_t left = req->content_len;
    while (left) {
        int n = recv_part(req, buf, left, sizeof buf);
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"avbruten\"}");
        }
        left -= n;
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(slot) != ESP_OK) {
        dash_log("Firmwaren är ogiltig, behåller den gamla");
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"ogiltig firmware\"}");
    }
    dash_log("Firmware sparad, startar om");
    send_json(req, "200 OK", "{\"ok\":true}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {          // handshake done: send the current fault codes
        uint8_t t[2 + 16 * 3];
        web_send(fd, MSG_DTC, t, car_dtc(t, sizeof t));
        dash_log("App ansluten");
        return ESP_OK;
    }
    uint8_t buf[128];
    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len > sizeof buf) return ESP_FAIL;    // not a command; drop the connection
    f.payload = buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK && (f.type == HTTPD_WS_TYPE_BINARY || f.type == HTTPD_WS_TYPE_TEXT))
        car_command(fd, buf, f.len);
    return err;
}

void web_start(void)
{
    if (access(APP, F_OK) != 0) rename(APP_NEW, APP);     // an app swap cut short by a reset
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_open_sockets = MAX_SOCKETS;
    c.lru_purge_enable = true;
    c.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&server, &c));
    static const httpd_uri_t uris[] = {
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
        { .uri = "/api/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/app", .method = HTTP_POST, .handler = app_post },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/*", .method = HTTP_GET, .handler = file_get },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "Webbserver på port 80");
}
