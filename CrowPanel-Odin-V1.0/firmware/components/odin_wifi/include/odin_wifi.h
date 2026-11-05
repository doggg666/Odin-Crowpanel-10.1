#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requires the V1.0 ESP32-C6 hosted Wi-Fi transport to be initialized first. */
esp_err_t odin_wifi_init(void);
esp_err_t odin_wifi_connect(const char *ssid, const char *password, bool save);
esp_err_t odin_wifi_disconnect(void);
esp_err_t odin_wifi_scan(wifi_ap_record_t *results, uint16_t *count);
bool odin_wifi_is_connected(void);
esp_err_t odin_wifi_connect_saved(void);

#ifdef __cplusplus
}
#endif
