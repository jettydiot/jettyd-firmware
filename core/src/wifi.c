/**
 * @file wifi.c
 * @brief WiFi management with retry and exponential backoff.
 */

#include "jettyd_wifi.h"
#include "jettyd_nvs.h"
#include "jettyd_provision.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "jettyd_wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static jettyd_wifi_state_t s_state = JETTYD_WIFI_DISCONNECTED;
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_state = JETTYD_WIFI_CONNECTING;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_state = JETTYD_WIFI_CONNECTING;
        s_retry_count++;

        /* Exponential backoff: min(init * 2^retry, max) */
        uint32_t delay_ms = JETTYD_WIFI_BACKOFF_INIT_MS;
        for (int i = 0; i < s_retry_count && delay_ms < JETTYD_WIFI_BACKOFF_MAX_MS; i++) {
            delay_ms *= 2;
        }
        if (delay_ms > JETTYD_WIFI_BACKOFF_MAX_MS) {
            delay_ms = JETTYD_WIFI_BACKOFF_MAX_MS;
        }

        ESP_LOGW(TAG, "WiFi disconnected, retry %d in %lu ms", s_retry_count, (unsigned long)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_state = JETTYD_WIFI_CONNECTED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t jettyd_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t jettyd_wifi_connect(void)
{
    const jettyd_provision_state_t *prov = jettyd_provision_get_state();
    if (prov == NULL || prov->wifi_ssid[0] == '\0') {
        ESP_LOGE(TAG, "No WiFi credentials in provisioning state");
        return ESP_ERR_INVALID_STATE;
    }

    return jettyd_wifi_connect_with(prov->wifi_ssid, prov->wifi_pass);
}

esp_err_t jettyd_wifi_connect_with(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    s_state = JETTYD_WIFI_FAILED;
    return ESP_FAIL;
}

esp_err_t jettyd_wifi_disconnect(void)
{
    s_state = JETTYD_WIFI_DISCONNECTED;
    return esp_wifi_disconnect();
}

jettyd_wifi_state_t jettyd_wifi_get_state(void)
{
    return s_state;
}

int8_t jettyd_wifi_get_rssi(void)
{
    if (s_state != JETTYD_WIFI_CONNECTED) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool jettyd_wifi_is_connected(void)
{
    return s_state == JETTYD_WIFI_CONNECTED;
}
