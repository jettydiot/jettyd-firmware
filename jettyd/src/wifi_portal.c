/**
 * @file wifi_portal.c
 * @brief SoftAP fallback config portal for WiFi recovery (FLU-164).
 *
 * Structure:
 *  1. Pure helpers — always compiled, host-testable (no ESP-IDF deps).
 *  2. Credential-persist seam — compiled when portal enabled or host-test.
 *  3. SoftAP + HTTP server — compiled only when CONFIG_JETTYD_WIFI_PORTAL=y
 *     AND CONFIG_SOC_WIFI_SUPPORTED (target builds only).
 *
 * When CONFIG_JETTYD_WIFI_PORTAL=n this translation unit contributes no data
 * or callable code to the final binary (the pure helpers are unreferenced and
 * eliminated by --gc-sections).
 */

#include "jettyd_wifi_portal.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "jettyd_portal";

/* ── Pure helpers (always compiled) ─────────────────────────────────────── */

bool jettyd_portal_should_trigger(int fail_count, int threshold, bool armed)
{
    return (fail_count >= threshold) && armed;
}

bool jettyd_portal_is_armed(bool no_ssid, bool in_boot_window, bool button_held_3s)
{
    return no_ssid || in_boot_window || button_held_3s;
}

esp_err_t jettyd_portal_validate_creds(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len > JETTYD_PORTAL_SSID_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (password != NULL && strlen(password) > JETTYD_PORTAL_PASS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* URL-decode one field value into out (NUL-terminated), stopping at '&' or end.
 * Returns pointer to position after the field in body (at '&', '\0', or end). */
static const char *decode_field(const char *src, const char *end,
                                char *out, size_t out_size)
{
    size_t i = 0;
    while (src < end && *src != '&') {
        if (i >= out_size - 1) {
            /* Overflow — consume remaining to advance cursor */
            while (src < end && *src != '&') {
                src++;
            }
            break;
        }
        if (*src == '+') {
            out[i++] = ' ';
            src++;
        } else if (*src == '%' && src + 2 < end && isxdigit((unsigned char)src[1])
                   && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            out[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            out[i++] = *src++;
        }
    }
    out[i] = '\0';
    return src;
}

/* Find "key=" in body and decode its value into out. Returns true if found. */
static bool extract_field(const char *body, size_t body_len, const char *key,
                          char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *end = body + body_len;
    const char *p = body;

    out[0] = '\0';
    while (p < end) {
        if ((size_t)(end - p) >= key_len && memcmp(p, key, key_len) == 0) {
            decode_field(p + key_len, end, out, out_size);
            return true;
        }
        /* Advance to next field */
        while (p < end && *p != '&') {
            p++;
        }
        if (p < end) {
            p++; /* skip '&' */
        }
    }
    return false;
}

esp_err_t jettyd_portal_parse_post_body(const char *body, size_t body_len,
                                        char *ssid_out, char *pass_out)
{
    if (!body || !ssid_out || !pass_out) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid_out[0] = '\0';
    pass_out[0] = '\0';

    extract_field(body, body_len, "ssid=",
                  ssid_out, JETTYD_PORTAL_SSID_MAX + 1);
    extract_field(body, body_len, "password=",
                  pass_out, JETTYD_PORTAL_PASS_MAX + 1);

    return jettyd_portal_validate_creds(ssid_out, pass_out[0] ? pass_out : NULL);
}

void jettyd_portal_make_ap_ssid(const char *device_id, char *out, size_t out_len)
{
    if (!device_id || device_id[0] == '\0') {
        snprintf(out, out_len, "jettyd-000000");
        return;
    }
    size_t id_len = strlen(device_id);
    const char *suffix = (id_len > 6) ? (device_id + id_len - 6) : device_id;
    snprintf(out, out_len, "jettyd-%s", suffix);
}

bool jettyd_portal_is_timed_out(int64_t last_activity_us, uint32_t timeout_s)
{
    int64_t now = esp_timer_get_time();
    return (now - last_activity_us) >= (int64_t)timeout_s * 1000000LL;
}

/* ── Credential-persist seam (portal enabled or host-test) ──────────────── */

#if CONFIG_JETTYD_WIFI_PORTAL || defined(JETTYD_PORTAL_HOST_TEST)

#include "jettyd_nvs.h"
#include "jettyd_provision.h"

esp_err_t jettyd_portal_save_creds(const char *ssid, const char *password)
{
    esp_err_t err = jettyd_portal_validate_creds(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    const char *pass = (password != NULL) ? password : "";

    err = jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE,
                               JETTYD_PROV_KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) {
        return err;
    }
    return jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE,
                                JETTYD_PROV_KEY_WIFI_PASS, pass);
}

#endif /* CONFIG_JETTYD_WIFI_PORTAL || JETTYD_PORTAL_HOST_TEST */

/* ── SoftAP + HTTP server (on-target, portal enabled) ──────────────────── */

#if CONFIG_JETTYD_WIFI_PORTAL && CONFIG_SOC_WIFI_SUPPORTED

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Embedded HTML pages ────────────────────────────────────────────────── */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>jettyd WiFi Setup</title>"
    "</head><body>"
    "<h2>WiFi Setup</h2>"
    "<div id=n></div>"
    "<form method=POST action=/save>"
    "SSID:<br><input id=s name=ssid maxlength=32 required><br>"
    "Password:<br><input name=password type=password maxlength=64><br><br>"
    "<button type=submit>Save &amp; Reboot</button>"
    "</form>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(function(ns){"
    "var d=document.getElementById('n');"
    "ns.forEach(function(n){"
    "var b=document.createElement('button');"
    "b.type='button';"
    "b.textContent=n.ssid;"
    "b.onclick=function(){document.getElementById('s').value=n.ssid;};"
    "d.appendChild(b);d.appendChild(document.createElement('br'));"
    "});});"
    "</script></body></html>";

static const char PORTAL_SAVED_HTML[] =
    "<!DOCTYPE html><html><body>"
    "<h2>Saved!</h2>"
    "<p>WiFi credentials saved. Rebooting...</p>"
    "</body></html>";

static const char PORTAL_ERROR_HTML[] =
    "<!DOCTYPE html><html><body>"
    "<h2>Error</h2>"
    "<p>Invalid credentials. Please try again.</p>"
    "<a href='/'>Back</a>"
    "</body></html>";

/* ── Scan cache ─────────────────────────────────────────────────────────── */

#define PORTAL_SCAN_MAX 10

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;
} portal_scan_entry_t;

static portal_scan_entry_t s_scan_cache[PORTAL_SCAN_MAX];
static int                 s_scan_count;

static void do_pre_ap_scan(void)
{
    s_scan_count = 0;
    wifi_scan_config_t scan_cfg = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Pre-AP scan failed");
        return;
    }

    uint16_t ap_count = PORTAL_SCAN_MAX;
    wifi_ap_record_t ap_records[PORTAL_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) != ESP_OK) {
        return;
    }

    for (int i = 0; i < (int)ap_count && i < PORTAL_SCAN_MAX; i++) {
        strlcpy(s_scan_cache[i].ssid, (char *)ap_records[i].ssid, 33);
        s_scan_cache[i].rssi     = ap_records[i].rssi;
        s_scan_cache[i].authmode = ap_records[i].authmode;
    }
    s_scan_count = (int)ap_count;
    ESP_LOGI(TAG, "Cached %d networks before AP switch", s_scan_count);
}

/* ── Portal state ───────────────────────────────────────────────────────── */

static volatile int64_t   s_last_activity_us;
static volatile bool      s_reboot_requested;
static httpd_handle_t     s_server;

/* ── HTTP handlers ──────────────────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, (int)strlen(PORTAL_HTML));
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < s_scan_count && n < (int)sizeof(buf) - 80; i++) {
        if (i > 0) {
            n += snprintf(buf + n, sizeof(buf) - n, ",");
        }
        n += snprintf(buf + n, sizeof(buf) - n,
                      "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                      s_scan_cache[i].ssid,
                      s_scan_cache[i].rssi,
                      s_scan_cache[i].authmode);
    }
    snprintf(buf + n, sizeof(buf) - n, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (int)strlen(buf));
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();

    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, PORTAL_ERROR_HTML, (int)strlen(PORTAL_ERROR_HTML));
        return ESP_OK;
    }
    body[received] = '\0';

    char ssid[JETTYD_PORTAL_SSID_MAX + 1] = {0};
    char pass[JETTYD_PORTAL_PASS_MAX + 1] = {0};

    esp_err_t err = jettyd_portal_parse_post_body(body, (size_t)received, ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Portal: invalid credentials in POST body");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PORTAL_ERROR_HTML, (int)strlen(PORTAL_ERROR_HTML));
        return ESP_OK;
    }

    err = jettyd_portal_save_creds(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Portal: NVS write failed (%d)", err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PORTAL_ERROR_HTML, (int)strlen(PORTAL_ERROR_HTML));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Portal: credentials saved for SSID '%s', rebooting in 2 s", ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_SAVED_HTML, (int)strlen(PORTAL_SAVED_HTML));

    s_reboot_requested = true;
    return ESP_OK;
}

/* ── SoftAP bring-up ────────────────────────────────────────────────────── */

static esp_err_t start_softap(const char *ap_ssid)
{
    esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len      = (uint8_t)strlen(ap_ssid);
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.channel       = 1;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up: SSID=%s — connect to http://192.168.4.1", ap_ssid);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t jettyd_portal_start(const char *device_id)
{
    /* Cache scan results while still in STA mode */
    do_pre_ap_scan();

    /* Build AP SSID */
    char ap_ssid[33];
    jettyd_portal_make_ap_ssid(device_id ? device_id : "", ap_ssid, sizeof(ap_ssid));

    /* Switch to SoftAP */
    start_softap(ap_ssid);

    /* Start HTTP server */
    s_last_activity_us = esp_timer_get_time();
    s_reboot_requested = false;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_server, &http_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        esp_restart();
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler
    };
    static const httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler
    };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_save);

#ifndef CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S
#define CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S 600
#endif

    /* Block until credentials saved (→ reboot) or idle timeout (→ reboot) */
    while (!s_reboot_requested) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (jettyd_portal_is_timed_out(s_last_activity_us,
                CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S)) {
            ESP_LOGW(TAG, "Portal idle timeout (%u s) — rebooting to retry station",
                     (unsigned)CONFIG_JETTYD_WIFI_PORTAL_TIMEOUT_S);
            break;
        }
    }

    if (s_reboot_requested) {
        /* Brief delay to let the HTTP response flush */
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    httpd_stop(s_server);
    s_server = NULL;

    ESP_LOGI(TAG, "Portal done — rebooting");
    esp_restart();

    /* Never reached */
    return ESP_OK;
}

#endif /* CONFIG_JETTYD_WIFI_PORTAL && CONFIG_SOC_WIFI_SUPPORTED */
