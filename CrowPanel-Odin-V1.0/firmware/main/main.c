#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"

static const char *TAG = "odin";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Odin firmware scaffold booted.");
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Target: %s, silicon revision: %u",
             CONFIG_IDF_TARGET, (unsigned)chip.revision);
    ESP_LOGW(TAG, "CrowPanel V1.0 board peripherals are not initialized yet.");
    ESP_LOGW(TAG, "Integrate and validate the V1.0 BSP with ESP-IDF 6.0.1 before using hardware.");
}
