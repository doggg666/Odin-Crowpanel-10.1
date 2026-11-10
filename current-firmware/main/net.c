#include "net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "settings.h"
#include "remote.h"
#include "ui.h"

static const char *TAG = "net";
#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_worker, s_weather;
static atomic_bool s_conn, s_changing;
static bool s_sntp;
static char s_ip[16] = "--", s_ap_name[32];
static bool s_scan_requested, s_connect_requested, s_pending_credentials, s_save_credentials;
static char s_pending_ssid[33], s_pending_password[65];
static char s_attempt_ssid[33], s_attempt_password[65];
static char s_connected_ssid[33], s_connected_password[65];
static wifi_ap_record_t s_aps[24];
static uint16_t s_ap_count;
static int s_scan_state; /* 0 idle, 1 scanning, 2 complete, 3 error */

static void wifi_config(wifi_config_t *wc, const char *ssid, const char *password)
{
    memset(wc, 0, sizeof *wc);
    size_t n = strlen(ssid); if (n > sizeof wc->sta.ssid) n = sizeof wc->sta.ssid;
    memcpy(wc->sta.ssid, ssid, n);
    n = strlen(password); if (n > sizeof wc->sta.password) n = sizeof wc->sta.password;
    memcpy(wc->sta.password, password, n);
    wc->sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc->sta.pmf_cfg.capable = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) esp_wifi_connect();
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            atomic_store(&s_conn, false);
            xEventGroupClearBits(s_ev, GOT_IP_BIT);
            strcpy(s_ip, "--");
            if (!atomic_load(&s_changing)) esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        atomic_store(&s_conn, true);
        xEventGroupSetBits(s_ev, GOT_IP_BIT);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pending_credentials) {
            strlcpy(s_connected_ssid, s_attempt_ssid, sizeof s_connected_ssid);
            strlcpy(s_connected_password, s_attempt_password, sizeof s_connected_password);
            s_save_credentials = true;
            s_pending_credentials = false;
        }
        xSemaphoreGive(s_lock);
        /* Keep flash writes out of the small system event-loop task stack. */
        if (s_worker) xTaskNotifyGive(s_worker);
        if (!s_sntp) {
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&c) == ESP_OK) s_sntp = true;
        }
        net_weather_refresh();
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

bool net_connected(void) { return atomic_load(&s_conn); }
const char *net_ip(void) { return s_ip; }
const char *net_setup_ssid(void) { return s_ap_name; }
int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (net_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool net_scan_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_scan_state != 1;
    if (ok) { s_scan_requested = true; s_scan_state = 1; s_ap_count = 0; }
    xSemaphoreGive(s_lock);
    if (ok) xTaskNotifyGive(s_worker);
    return ok;
}
cJSON *net_scan_results(void)
{
    cJSON *root = cJSON_CreateObject(), *aps = cJSON_AddArrayToObject(root, "networks");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddNumberToObject(root, "state", s_scan_state);
    for (unsigned i = 0; i < s_ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        char ssid[33]; memcpy(ssid, s_aps[i].ssid, 32); ssid[32] = 0;
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", s_aps[i].rssi);
        cJSON_AddBoolToObject(ap, "secured", s_aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(aps, ap);
    }
    xSemaphoreGive(s_lock);
    return root;
}
esp_err_t net_connect_request(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64 ||
        (password[0] && strlen(password) < 8)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_pending_ssid, ssid, sizeof s_pending_ssid);
    strlcpy(s_pending_password, password, sizeof s_pending_password);
    s_connect_requested = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_worker);
    return ESP_OK;
}

static void network_worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool scan = s_scan_requested, connect = s_connect_requested, save = s_save_credentials;
        s_scan_requested = s_connect_requested = s_save_credentials = false;
        char connected_ssid[33], connected_password[65];
        strlcpy(connected_ssid, s_connected_ssid, sizeof connected_ssid);
        strlcpy(connected_password, s_connected_password, sizeof connected_password);
        char ssid[33], password[65];
        strlcpy(ssid, s_pending_ssid, sizeof ssid); strlcpy(password, s_pending_password, sizeof password);
        xSemaphoreGive(s_lock);
        if (save) {
            settings_lock();
            strlcpy(g_cfg.wifi_ssid, connected_ssid, sizeof g_cfg.wifi_ssid);
            strlcpy(g_cfg.wifi_password, connected_password, sizeof g_cfg.wifi_password);
            settings_save(); settings_unlock();
        }
        if (scan) {
            wifi_scan_config_t sc = {.show_hidden = false};
            esp_err_t err = esp_wifi_scan_start(&sc, true);
            wifi_ap_record_t *records = calloc(24, sizeof *records);
            uint16_t count = 24;
            if (err == ESP_OK && records) err = esp_wifi_scan_get_ap_records(&count, records);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (err == ESP_OK && records) { memcpy(s_aps, records, count * sizeof *records); s_ap_count = count; s_scan_state = 2; }
            else s_scan_state = 3;
            xSemaphoreGive(s_lock); free(records);
        }
        if (connect) {
            atomic_store(&s_changing, true);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            wifi_config_t wc; wifi_config(&wc, ssid, password);
            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_pending_credentials = err == ESP_OK;
            /* Credentials are saved only after DHCP succeeds. */
            if (s_pending_credentials) {
                strlcpy(s_attempt_ssid, ssid, sizeof s_attempt_ssid);
                strlcpy(s_attempt_password, password, sizeof s_attempt_password);
            }
            xSemaphoreGive(s_lock);
            atomic_store(&s_changing, false);
            if (err == ESP_OK) esp_wifi_connect();
        }
        /* A command can arrive while a scan or connection change is running. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool again = s_scan_requested || s_connect_requested || s_save_credentials;
        xSemaphoreGive(s_lock);
        if (again) xTaskNotifyGive(s_worker);
    }
}

static const char *wmo_text(int c)
{
    if (c == 0) return "Clear sky";
    if (c <= 2) return "Partly cloudy";
    if (c == 3) return "Overcast";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Rain showers";
    if (c == 85 || c == 86) return "Snow showers";
    if (c >= 95) return "Thunderstorm";
    return "--";
}
void net_weather_refresh(void) { if (s_weather) xTaskNotifyGive(s_weather); }
cJSON *net_location_search(const char *city, const char *country)
{
    char query[160], encoded[512], url[700];
    snprintf(query, sizeof query, "%s%s%s", city, country[0] ? ", " : "", country);
    if (!remote_encode(encoded, sizeof encoded, query)) return NULL;
    snprintf(url, sizeof url, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=en&format=json", encoded);
    return remote_json(url, NULL, NULL, NULL);
}
static bool fetch_weather(void)
{
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return false;
    settings_snapshot(cfg);
    char url[380];
    snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
        "&temperature_unit=celsius&wind_speed_unit=kmh&timezone=auto", cfg->weather_lat, cfg->weather_lon);
    int status = 0;
    cJSON *root = remote_json(url, NULL, NULL, &status);
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m"), *fl = cJSON_GetObjectItem(cur, "apparent_temperature");
    cJSON *h = cJSON_GetObjectItem(cur, "relative_humidity_2m"), *w = cJSON_GetObjectItem(cur, "wind_speed_10m");
    cJSON *wc = cJSON_GetObjectItem(cur, "weather_code");
    bool ok = cJSON_IsNumber(t) && cJSON_IsNumber(fl) && cJSON_IsNumber(h) && cJSON_IsNumber(w) && cJSON_IsNumber(wc);
    /* Do not publish a response for a location that changed during the request. */
    settings_lock();
    bool current = cfg->weather_lat == g_cfg.weather_lat && cfg->weather_lon == g_cfg.weather_lon;
    settings_unlock();
    if (ok && current) {
        ui_set_weather_location(cfg->weather_city, cfg->weather_country_code);
        ui_set_weather(true, t->valuedouble, fl->valuedouble, h->valueint, w->valuedouble, wmo_text(wc->valueint));
        ESP_LOGI(TAG, "weather updated: %.1f C", t->valuedouble);
    } else if (!ok) ESP_LOGW(TAG, "weather request failed (HTTP %d)", status);
    cJSON_Delete(root); free(cfg);
    return ok;
}
static void weather_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        bool ok = fetch_weather();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? 300000 : 30000));
    }
}
void net_start(void)
{
    s_ev = xEventGroupCreate(); s_lock = xSemaphoreCreateMutex();
    configASSERT(s_ev && s_lock);
    setenv("TZ", CONFIG_ODIN_TIMEZONE, 1); tzset();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    odin_settings_t *cfg = malloc(sizeof *cfg); configASSERT(cfg); settings_snapshot(cfg);
    wifi_config_t wc; wifi_config(&wc, cfg->wifi_ssid, cfg->wifi_password);
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_ap_name, sizeof s_ap_name, "Odin-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_name, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(s_ap_name); ap.ap.channel = 1;
    ap.ap.max_connection = 4; ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap.ap.password, cfg->web_password, sizeof ap.ap.password);
    free(cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    xTaskCreate(network_worker, "network_cfg", 8192, NULL, 3, &s_worker);
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, &s_weather);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "setup network: %s, web server at 192.168.4.1", s_ap_name);
}
