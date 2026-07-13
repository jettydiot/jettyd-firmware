/**
 * @file esp_http_client.h
 * @brief Host-test mock for esp_http_client (ESP-IDF).
 *
 * Stubs the PUT upload path used by the camera upload task.
 * Test files define the extern control variables below.
 */

#pragma once
#include "esp_idf_stubs.h"
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------ */

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_PUT,
    HTTP_METHOD_POST,
    HTTP_METHOD_DELETE,
} esp_http_client_method_t;

typedef struct {
    const char             *url;
    esp_http_client_method_t method;
    int                     timeout_ms;
} esp_http_client_config_t;

typedef void *esp_http_client_handle_t;

/* ------------------------------------------------------------------
 * Test-control globals (defined in the test file)
 * ------------------------------------------------------------------ */

extern int    g_http_status_code;       /**< Status code returned by get_status_code */
extern size_t g_http_bytes_written;     /**< Accumulated bytes written via client_write */
extern bool   g_http_open_ok;           /**< Whether esp_http_client_open succeeds */
extern char   g_http_last_url[512];     /**< Last URL passed to esp_http_client_init */

/* Static dummy handle — non-NULL so `if (!client)` guards pass */
static int s_http_dummy_handle;

/* ------------------------------------------------------------------
 * Inline stubs
 * ------------------------------------------------------------------ */

static inline esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *cfg)
{
    if (cfg && cfg->url) {
        strncpy(g_http_last_url, cfg->url, sizeof(g_http_last_url) - 1);
    }
    return (esp_http_client_handle_t)&s_http_dummy_handle;
}

static inline esp_err_t
esp_http_client_set_header(esp_http_client_handle_t c,
                           const char *k, const char *v)
{
    (void)c; (void)k; (void)v;
    return ESP_OK;
}

static inline esp_err_t
esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)c; (void)write_len;
    return g_http_open_ok ? ESP_OK : ESP_FAIL;
}

static inline int
esp_http_client_write(esp_http_client_handle_t c,
                      const char *buf, int len)
{
    (void)c; (void)buf;
    if (len > 0) g_http_bytes_written += (size_t)len;
    return len;
}

static inline int
esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    (void)c;
    return g_http_status_code;
}

static inline esp_err_t
esp_http_client_cleanup(esp_http_client_handle_t c)
{
    (void)c;
    return ESP_OK;
}
