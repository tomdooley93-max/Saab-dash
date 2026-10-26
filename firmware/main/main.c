#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "dash.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_vfs_spiffs_conf_t www = { .base_path = "/www", .partition_label = "storage",
                                  .max_files = 4, .format_if_mount_failed = true };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&www));

    net_start();
    web_start();
    car_start();

    // Reaching this point means the new firmware boots and serves; keep it.
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI("main", "Saab Dash %s igång", esp_app_get_description()->version);
}
