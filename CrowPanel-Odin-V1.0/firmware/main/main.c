#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "odin";

void app_main(void)
{
    ESP_LOGI(TAG, "Odin firmware scaffold booted.");
    ESP_LOGI(TAG, "Chip: %s, revision: %d", CONFIG_IDF_TARGET,
             esp_get_chip_revision());
    ESP_LOGW(TAG,
             "Board peripherals are not initialized in this scaffold yet.");
    ESP_LOGW(TAG,
             "Next: integrate the exact Elecrow CrowPanel Advanced 10.1 V1.0 BSP.");
}
