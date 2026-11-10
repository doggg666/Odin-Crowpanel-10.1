#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "board.h"
#include "camera.h"
#include "web.h"
#include "microphone.h"
#include "settings.h"
#include "net.h"
#include "odin.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "main";

static const char *rst_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int-watchdog";
    case ESP_RST_TASK_WDT:   return "task-watchdog";
    case ESP_RST_WDT:        return "other-watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep-sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    default:                 return "unknown";
    }
}

static void log_boot_count(void)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_get_u32(h, "count", &n);
    ESP_LOGW("main", "reset: %s (%d), boots this flash: %u",
             rst_name(esp_reset_reason()), (int)esp_reset_reason(), (unsigned)(n + 1));
    nvs_set_u32(h, "count", n + 1);
    nvs_commit(h);
    nvs_close(h);
}

/*
 * ESP-IDF 5.5 on ESP32-P4 registers the 8 KB HP-TCM as a general-purpose
 * internal heap. Heaps are sorted by size (smallest first), so small
 * MALLOC_CAP_INTERNAL allocations are served from TCM before normal SRAM.
 * TCM is not part of the byte-accessible/internal ranges FreeRTOS uses for
 * task memory, so a task stack that lands there trips
 * xPortcheckValidStackMem() in xTaskCreateStaticPinnedToCore().
 *
 * ESP-Hosted 2.12/2.13 changes the early allocation pattern enough that the
 * FreeRTOS timer-service task stack ends up in TCM and the board panics
 * before app_main(). Reserve TCM before the scheduler starts so it can never
 * be handed out as a task stack.
 */
#define TCM_RESERVE_CHUNK 128
#define TCM_RESERVE_MAX   128
static void *s_tcm_reserved[TCM_RESERVE_MAX];

static void __attribute__((constructor)) reserve_tcm_for_stacks(void)
{
    size_t n = 0;
    while (n < TCM_RESERVE_MAX) {
        void *p = heap_caps_malloc(TCM_RESERVE_CHUNK, MALLOC_CAP_TCM);
        if (!p) {
            break;
        }
        s_tcm_reserved[n++] = p;
    }
    ESP_EARLY_LOGI(TAG, "reserved %u bytes of TCM to keep task stacks out of it",
                   (unsigned)(n * TCM_RESERVE_CHUNK));
}

static void status_task(void *arg)
{
    int rssi = 0;
    for (int tick = 0;; tick++) {
        time_t now;
        struct tm tm;
        time(&now);
        localtime_r(&now, &tm);
        ui_set_clock(tm.tm_year >= 124 ? &tm : NULL);   /* year >= 2024 means SNTP synced */

        if (tick % 5 == 0) rssi = net_rssi();
        ui_set_wifi(net_connected(), rssi);
        ui_set_ip(net_ip());
        ui_set_llm(odin_llm_online());
        ui_set_tts(odin_tts_state());
        if (tick % 10 == 2) {
            int millivolts;
            int percent = board_battery_percent(&millivolts);
            ui_set_battery(percent, millivolts);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_boot_count();
    settings_load();
    ESP_ERROR_CHECK(board_init());
    board_set_volume(g_cfg.volume);

    ui_init();
    ui_apply_display();          /* lights the backlight after the first frame is queued */
    microphone_init();
    camera_init();

    net_start();
    odin_init();
    web_start();
    xTaskCreate(status_task, "status", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Odin dashboard running");
}
