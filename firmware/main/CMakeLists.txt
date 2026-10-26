idf_component_register(SRCS "main.c" "net.c" "web.c" "car.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_app_format esp_event esp_wifi esp_netif esp_http_server esp_timer app_update spiffs nvs_flash)
