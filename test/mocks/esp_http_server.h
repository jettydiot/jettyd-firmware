/*
 * Host-test stub for esp_http_server.h (ESP-IDF).
 *
 * Provides the minimal type surface needed to compile wifi_portal.c on the
 * host. None of these functions are called from host-test code paths — the
 * SoftAP/HTTP server block is guarded by CONFIG_SOC_WIFI_SUPPORTED.
 */
#pragma once
#include "esp_idf_stubs.h"

typedef void *httpd_handle_t;

typedef enum {
    HTTP_GET  = 0,
    HTTP_POST = 3,
} http_method_t;
#define HTTP_GET  HTTP_GET
#define HTTP_POST HTTP_POST

typedef struct {
    const char *uri;
    http_method_t method;
    esp_err_t (*handler)(struct httpd_req *req);
    void *user_ctx;
} httpd_uri_t;

typedef struct httpd_req {
    const char *uri;
    void *user_ctx;
    size_t content_len;
    char *aux;
} httpd_req_t;

typedef struct {
    int server_port;
    int ctrl_port;
    int max_open_sockets;
    int max_uri_handlers;
    int backlog_conn;
    bool lru_purge_enable;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { \
    .server_port = 80, \
    .ctrl_port = 32768, \
    .max_open_sockets = 7, \
    .max_uri_handlers = 8, \
    .backlog_conn = 5, \
    .lru_purge_enable = false, \
}

#define ESP_ERR_HTTPD_BASE          0x9000
#define ESP_ERR_HTTPD_HANDLERS_FULL (ESP_ERR_HTTPD_BASE + 1)
#define ESP_ERR_HTTPD_INVALID_REQ   (ESP_ERR_HTTPD_BASE + 2)

static inline esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c)
    { (void)h; (void)c; return ESP_OK; }
static inline esp_err_t httpd_stop(httpd_handle_t h)
    { (void)h; return ESP_OK; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u)
    { (void)h; (void)u; return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, int len)
    { (void)r; (void)buf; (void)len; return ESP_OK; }
static inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t)
    { (void)r; (void)t; return ESP_OK; }
static inline esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s)
    { (void)r; (void)s; return ESP_OK; }
static inline int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
    { (void)r; (void)buf; (void)len; return 0; }
static inline size_t httpd_req_get_url_query_len(httpd_req_t *r)
    { (void)r; return 0; }
