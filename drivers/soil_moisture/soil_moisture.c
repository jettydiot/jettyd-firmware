/**
 * @file soil_moisture.c
 * @brief Soil moisture sensor via ADC — maps raw reading to 0-100%.
 */

#include "soil_moisture.h"
#include "jettyd_driver.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv_soil";

static soil_moisture_config_t s_cfg;
static jettyd_driver_t s_driver;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_channel_t s_channel;

/**
 * Map a GPIO pin to an ADC1 channel. This is chip-specific;
 * we cover the common ESP32-S3 mappings here.
 */
static adc_channel_t pin_to_channel(uint8_t pin)
{
    /* ESP32-S3: ADC1 channels map to GPIO 1-10 */
    if (pin >= 1 && pin <= 10) {
        return (adc_channel_t)(pin - 1);
    }
    /* ESP32: ADC1 channels map to GPIO 32-39 */
    if (pin >= 32 && pin <= 39) {
        return (adc_channel_t)(pin - 32);
    }
    return ADC_CHANNEL_0;
}

static esp_err_t soil_init(const void *config)
{
    const soil_moisture_config_t *c = (const soil_moisture_config_t *)config;
    s_cfg = *c;
    s_channel = pin_to_channel(s_cfg.pin);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Soil moisture init: pin=%d, ch=%d, dry=%u, wet=%u",
             s_cfg.pin, s_channel, s_cfg.dry_value, s_cfg.wet_value);
    return ESP_OK;
}

static esp_err_t soil_deinit(void)
{
    if (s_adc_handle) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
    }
    return ESP_OK;
}

static jettyd_value_t soil_read(const char *capability)
{
    jettyd_value_t val = {.type = JETTYD_VAL_FLOAT, .valid = false};

    if (s_adc_handle == NULL) {
        return val;
    }

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, s_channel, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
        return val;
    }

    /* Map raw ADC to percentage: dry_value=0%, wet_value=100% */
    float pct = 100.0f * (float)(s_cfg.dry_value - raw)
                / (float)(s_cfg.dry_value - s_cfg.wet_value);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    val.float_val = pct;
    val.valid = true;
    return val;
}

static esp_err_t soil_self_test(void)
{
    if (s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int raw = 0;
    return adc_oneshot_read(s_adc_handle, s_channel, &raw);
}

void soil_moisture_register(const char *instance, const void *config)
{
    soil_init(config);

    memset(&s_driver, 0, sizeof(s_driver));
    strncpy(s_driver.instance, instance, JETTYD_MAX_INSTANCE_NAME - 1);
    strlcpy(s_driver.driver_name, "soil_moisture", sizeof(s_driver.driver_name));

    s_driver.capability_count = 1;
    strlcpy(s_driver.capabilities[0].name, "moisture", sizeof(s_driver.capabilities[0].name));
    s_driver.capabilities[0].type = JETTYD_CAP_READABLE;
    s_driver.capabilities[0].value_type = JETTYD_VAL_FLOAT;
    s_driver.capabilities[0].min_value = 0;
    s_driver.capabilities[0].max_value = 100;
    strlcpy(s_driver.capabilities[0].unit, "%", sizeof(s_driver.capabilities[0].unit));

    s_driver.init = soil_init;
    s_driver.deinit = soil_deinit;
    s_driver.read = soil_read;
    s_driver.self_test = soil_self_test;

    JETTYD_REGISTER_DRIVER(&s_driver);
}
