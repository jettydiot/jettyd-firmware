/**
 * @file provision.c
 * @brief Fleet provisioning: exchange fleet token for device key.
 */

#include "jettyd_provision.h"
#include "jettyd_nvs.h"
#include "jettyd_mqtt.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "jettyd_prov";

#define PROV_RESPONSE_BIT BIT0
#define PROV_TIMEOUT_MS   30000

static jettyd_provision_state_t s_state = {0};
static EventGroupHandle_t s_prov_event_group = NULL;

/* Temporary storage for provisioning response */
static char s_resp_device_id[64];
static char s_resp_device_key[128];
static char s_resp_tenant_id[64];

static void provision_response_handler(const char *topic, const char *data, int data_len)
{
    ESP_LOGI(TAG, "Provisioning response received");

    /* data is MQTT payload — size is bounded by MQTT message limit */
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse provisioning response");
        return;
    }

    cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    cJSON *device_key = cJSON_GetObjectItem(root, "device_key");
    cJSON *tenant_id = cJSON_GetObjectItem(root, "tenant_id");

    if (cJSON_IsString(device_id) && cJSON_IsString(device_key)) {
        strncpy(s_resp_device_id, device_id->valuestring, sizeof(s_resp_device_id) - 1);
        strncpy(s_resp_device_key, device_key->valuestring, sizeof(s_resp_device_key) - 1);
        if (cJSON_IsString(tenant_id)) {
            strncpy(s_resp_tenant_id, tenant_id->valuestring, sizeof(s_resp_tenant_id) - 1);
        }
        xEventGroupSetBits(s_prov_event_group, PROV_RESPONSE_BIT);
    } else {
        ESP_LOGE(TAG, "Invalid provisioning response format");
    }

    cJSON_Delete(root);
}

esp_err_t jettyd_provision_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    /* Read provisioning state from NVS */
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_TENANT_ID,
                        s_state.tenant_id, sizeof(s_state.tenant_id));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_ID,
                        s_state.device_id, sizeof(s_state.device_id));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_KEY,
                        s_state.device_key, sizeof(s_state.device_key));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_FLEET_TOKEN,
                        s_state.fleet_token, sizeof(s_state.fleet_token));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_MQTT_URI,
                        s_state.mqtt_uri, sizeof(s_state.mqtt_uri));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_WIFI_SSID,
                        s_state.wifi_ssid, sizeof(s_state.wifi_ssid));
    jettyd_nvs_read_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_WIFI_PASS,
                        s_state.wifi_pass, sizeof(s_state.wifi_pass));

    s_state.provisioned = (s_state.device_key[0] != '\0');

    ESP_LOGI(TAG, "Provision state: %s (tenant: %s)",
             s_state.provisioned ? "provisioned" : "not provisioned",
             s_state.tenant_id);

    return ESP_OK;
}

bool jettyd_provision_is_provisioned(void)
{
    return s_state.provisioned;
}

const jettyd_provision_state_t *jettyd_provision_get_state(void)
{
    return &s_state;
}

esp_err_t jettyd_provision_run(void)
{
    if (s_state.provisioned) {
        ESP_LOGI(TAG, "Already provisioned, skipping");
        return ESP_OK;
    }

    if (s_state.fleet_token[0] == '\0') {
        ESP_LOGE(TAG, "No fleet token available for provisioning");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting provisioning with fleet token");

    s_prov_event_group = xEventGroupCreate();
    memset(s_resp_device_id, 0, sizeof(s_resp_device_id));
    memset(s_resp_device_key, 0, sizeof(s_resp_device_key));
    memset(s_resp_tenant_id, 0, sizeof(s_resp_tenant_id));

    /* Subscribe to provisioning response topic (wildcard) */
    esp_err_t err = jettyd_mqtt_subscribe("jettyd/provision/response/#", 1,
                                           provision_response_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to provisioning response");
        vEventGroupDelete(s_prov_event_group);
        return err;
    }

    /* Build and publish provisioning request */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "fleet_token", s_state.fleet_token);
    cJSON_AddStringToObject(req, "device_type", JETTYD_DEVICE_TYPE ? JETTYD_DEVICE_TYPE : "unknown");
    cJSON_AddStringToObject(req, "firmware_version",
                            JETTYD_FIRMWARE_VERSION ? JETTYD_FIRMWARE_VERSION : "0.0.0");

    /* Include MAC address as a unique hardware identifier */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(req, "mac_address", mac_str);

    /* Use a static buffer to avoid dynamic allocation at runtime.
     * Fleet token + device info fits comfortably in 512 bytes. */
    static char json_buf[512];
    char *json_str_tmp = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (json_str_tmp == NULL) {
        vEventGroupDelete(s_prov_event_group);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(json_buf, json_str_tmp, sizeof(json_buf));
    cJSON_free(json_str_tmp);
    char *json_str = json_buf;

    err = jettyd_mqtt_publish("jettyd/provision/request", json_str, 1, false);
    /* json_str points to static buffer, no free needed */

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish provisioning request");
        vEventGroupDelete(s_prov_event_group);
        return err;
    }

    /* Wait for response */
    EventBits_t bits = xEventGroupWaitBits(s_prov_event_group, PROV_RESPONSE_BIT,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(PROV_TIMEOUT_MS));
    vEventGroupDelete(s_prov_event_group);
    s_prov_event_group = NULL;

    if (!(bits & PROV_RESPONSE_BIT)) {
        ESP_LOGE(TAG, "Provisioning timed out");
        return ESP_ERR_TIMEOUT;
    }

    /* Store credentials in state and NVS */
    strncpy(s_state.device_id, s_resp_device_id, sizeof(s_state.device_id) - 1);
    strncpy(s_state.device_key, s_resp_device_key, sizeof(s_state.device_key) - 1);
    if (s_resp_tenant_id[0] != '\0') {
        strncpy(s_state.tenant_id, s_resp_tenant_id, sizeof(s_state.tenant_id) - 1);
    }
    s_state.provisioned = true;

    jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_ID, s_state.device_id);
    jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_KEY, s_state.device_key);
    jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_TENANT_ID, s_state.tenant_id);

    /* Delete fleet token from NVS — it's single-use */
    jettyd_nvs_erase_key(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_FLEET_TOKEN);
    s_state.fleet_token[0] = '\0';

    ESP_LOGI(TAG, "Provisioned successfully: device_id=%s", s_state.device_id);

    /* Reconnect MQTT with device credentials */
    err = jettyd_mqtt_reconfigure(s_state.device_id, s_state.device_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconnect MQTT with device credentials");
    }

    return err;
}

esp_err_t jettyd_provision_store(const jettyd_provision_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->tenant_id[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_TENANT_ID, state->tenant_id);
    }
    if (state->device_id[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_ID, state->device_id);
    }
    if (state->device_key[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_DEVICE_KEY, state->device_key);
    }
    if (state->fleet_token[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_FLEET_TOKEN, state->fleet_token);
    }
    if (state->mqtt_uri[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_MQTT_URI, state->mqtt_uri);
    }
    if (state->wifi_ssid[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_WIFI_SSID, state->wifi_ssid);
    }
    if (state->wifi_pass[0]) {
        jettyd_nvs_write_str(JETTYD_PROV_NVS_NAMESPACE, JETTYD_PROV_KEY_WIFI_PASS, state->wifi_pass);
    }

    memcpy(&s_state, state, sizeof(jettyd_provision_state_t));
    return ESP_OK;
}

esp_err_t jettyd_provision_clear(void)
{
    memset(&s_state, 0, sizeof(s_state));
    return jettyd_nvs_erase_namespace(JETTYD_PROV_NVS_NAMESPACE);
}
