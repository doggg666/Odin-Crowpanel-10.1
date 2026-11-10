#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
void net_start(void);
bool net_connected(void);
int net_rssi(void);
const char *net_ip(void);
const char *net_setup_ssid(void);
bool net_scan_start(void);
cJSON *net_scan_results(void);
esp_err_t net_connect_request(const char *ssid, const char *password);
void net_weather_refresh(void);
cJSON *net_location_search(const char *city, const char *country);
