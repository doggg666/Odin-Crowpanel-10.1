#include "odin_wifi.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "odin_wifi";
static esp_netif_t *sta_netif;
static bool initialized;
static bool connected;
static bool intentional_disconnect;
static unsigned retries;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        if (!intentional_disconnect && retries++ < 5) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected; retries exhausted or requested");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        retries = 0;
        ESP_LOGI(TAG, "Wi-Fi connected; IP address acquired");
    }
}

esp_err_t odin_wifi_init(void)
{
    if (initialized) return ESP_OK;
    /* BSP must initialize ESP32-C6 SDIO/ESP-Hosted transport first. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) return ESP_FAIL;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err == ESP_OK) initialized = true;
    return err;
}

esp_err_t odin_wifi_connect(const char *ssid, const char *password, bool save)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 63)
        return ESP_ERR_INVALID_ARG;
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, password, strlen(password));
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    intentional_disconnect = true;
    connected = false;
    esp_wifi_disconnect();
    intentional_disconnect = false;
    retries = 0;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    if (save) {
        nvs_handle_t nvs;
        err = nvs_open("odin_wifi", NVS_READWRITE, &nvs);
        if (err != ESP_OK) return err;
        err = nvs_set_str(nvs, "ssid", ssid);
        if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return err;
}

esp_err_t odin_wifi_connect_saved(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("odin_wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    char ssid[33] = {0};
    char password[64] = {0};
    size_t slen = sizeof(ssid), plen = sizeof(password);
    err = nvs_get_str(nvs, "ssid", ssid, &slen);
    if (err == ESP_OK) err = nvs_get_str(nvs, "password", password, &plen);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    return odin_wifi_connect(ssid, password, false);
}

esp_err_t odin_wifi_disconnect(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    intentional_disconnect = true;
    connected = false;
    return esp_wifi_disconnect();
}

esp_err_t odin_wifi_scan(wifi_ap_record_t *results, uint16_t *count)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    if (!results || !count || *count == 0) return ESP_ERR_INVALID_ARG;
    wifi_scan_config_t cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) return err;
    return esp_wifi_scan_get_ap_records(count, results);
}

bool odin_wifi_is_connected(void)
{
    return connected;
}
