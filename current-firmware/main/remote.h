#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"
#include "esp_http_client.h"
bool remote_url(char *out, size_t size, const char *base, const char *suffix);
bool remote_write_all(esp_http_client_handle_t client, const void *data, size_t size);
void remote_auth(esp_http_client_handle_t client, const char *key);
cJSON *remote_json(const char *url, const char *key, const char *post, int *status);
bool remote_encode(char *out, size_t size, const char *text);
