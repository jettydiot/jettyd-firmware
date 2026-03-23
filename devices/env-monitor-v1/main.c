/**
 * @file main.c
 * @brief Environmental monitor v1 — entry point.
 */

#include "jettyd.h"
#include "driver_manifest.h"

void app_main(void)
{
    jettyd_config_t config = {
        .device_type = JETTYD_MANIFEST_DEVICE_TYPE,
        .firmware_version = JETTYD_MANIFEST_FIRMWARE_VERSION,
        .heartbeat_interval_sec = JETTYD_MANIFEST_HEARTBEAT_INTERVAL,
        .default_metrics = JETTYD_MANIFEST_DEFAULT_METRICS,
        .mqtt_keepalive = JETTYD_MANIFEST_MQTT_KEEPALIVE,
        .mqtt_qos = JETTYD_MANIFEST_MQTT_QOS,
        .mqtt_buffer_on_disconnect = JETTYD_MANIFEST_MQTT_BUFFER,
        .mqtt_max_buffer_size = JETTYD_MANIFEST_MQTT_MAX_BUFFER,
        .deep_sleep = JETTYD_MANIFEST_DEEP_SLEEP,
        .sleep_duration_sec = JETTYD_MANIFEST_SLEEP_DURATION,
        .wake_on_pin = -1,
        .has_battery = JETTYD_MANIFEST_HAS_BATTERY,
        .battery_adc_pin = JETTYD_MANIFEST_BATTERY_ADC_PIN,
        .battery_voltage_divider = JETTYD_MANIFEST_BATTERY_DIVIDER,
        .status_led_pin = JETTYD_MANIFEST_STATUS_LED_PIN,
    };

    jettyd_init(&config);
    jettyd_start();
}
