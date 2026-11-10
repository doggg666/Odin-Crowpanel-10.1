#include "remote.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"

bool remote_url(char *out, size_t size, const char *base, const char *suffix)
{
    if (!base || (strncmp(base, "http://", 7) && strncmp(base, "https://", 8))) return false;
    if (strlcpy(out, base, size) >= size) return false;
    size_t n = strlen(out);
    while (n && out[n - 1] == '/') out[--n] = 0;
    size_t s = strlen(suffix);
    if (n >= s && !strcmp(out + n - s, suffix)) return true;
    /* Accept either a server root or an API base ending in /v1. */
    if (n >= 3 && !strcmp(out + n - 3, "/v1") && !strncmp(suffix, "/v1/", 4)) suffix += 3;
    return strlcat(out, suffix, size) < size;
}
void remote_auth(esp_http_client_handle_t client, const char *key)
{
    if (!key || !key[0]) return;
    char auth[272];
    snprintf(auth, sizeof auth, "Bearer %s", key);
    esp_http_client_set_header(client, "Authorization", auth);
}
bool remote_write_all(esp_http_client_handle_t client, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        int n = esp_http_client_write(client, p, size > 4096 ? 4096 : size);
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
cJSON *remote_json(const char *url, const char *key, const char *post, int *status)
{
    if (status) *status = 0;
    esp_http_client_config_t cfg = {.url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = post ? 60000 : 12000, .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    remote_auth(c, key);
    if (strstr(url, "opencode.ai/")) {
        static char session[48];
        if (!session[0]) snprintf(session, sizeof session, "ses_odin_%08lx", (unsigned long)esp_random());
        esp_http_client_set_header(c, "x-opencode-session", session);
    }
    esp_http_client_set_header(c, "Accept", "application/json");
    if (post) esp_http_client_set_header(c, "Content-Type", "application/json");
    cJSON *result = NULL;
    if (esp_http_client_open(c, post ? strlen(post) : 0) == ESP_OK) {
        if (!post || remote_write_all(c, post, strlen(post))) {
            esp_http_client_fetch_headers(c);
            int code = esp_http_client_get_status_code(c);
            if (status) *status = code;
            if (code >= 200 && code < 300) {
                const size_t cap = 32768;
                char *buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (buf) {
                    size_t total = 0; int n;
                    while (total < cap && (n = esp_http_client_read(c, buf + total, cap - total)) > 0) total += n;
                    buf[total] = 0;
                    if (total < cap) result = cJSON_ParseWithLength(buf, total);
                    free(buf);
                }
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return result;
}
bool remote_encode(char *out, size_t size, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (n + 1 >= size) return false;
            out[n++] = *p;
        } else {
            if (n + 3 >= size) return false;
            out[n++] = '%'; out[n++] = hex[*p >> 4]; out[n++] = hex[*p & 15];
        }
    }
    if (n >= size) return false;
    out[n] = 0; return true;
}
