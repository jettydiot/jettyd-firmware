/**
 * @file jettyd_driver.h
 * @brief Driver registration interface for the Jettyd firmware SDK.
 *
 * Every peripheral driver implements this abstract interface. The core
 * runtime discovers drivers at boot via a registration table populated
 * by auto-generated driver_init.c.
 */

#ifndef JETTYD_DRIVER_H
#define JETTYD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

#define JETTYD_MAX_DRIVERS          16
#define JETTYD_MAX_CAPABILITIES     8
#define JETTYD_MAX_INSTANCE_NAME    16

typedef enum {
    JETTYD_CAP_READABLE,       /**< Can be read (sensor value) */
    JETTYD_CAP_WRITABLE,       /**< Can be set to a value (PWM, servo) */
    JETTYD_CAP_SWITCHABLE,     /**< Can be toggled on/off (relay, valve) */
    JETTYD_CAP_ALERTABLE,      /**< Can generate threshold alerts */
} jettyd_capability_type_t;

typedef enum {
    JETTYD_VAL_FLOAT,
    JETTYD_VAL_INT,
    JETTYD_VAL_BOOL,
    JETTYD_VAL_STRING,
} jettyd_value_type_t;

typedef struct {
    char name[32];                      /**< e.g., "moisture", "temperature" */
    jettyd_capability_type_t type;
    jettyd_value_type_t value_type;
    float min_value;                    /**< For validation */
    float max_value;
    char unit[8];                       /**< e.g., "%", "°C", "V" */
} jettyd_capability_t;

typedef struct {
    float float_val;
    int32_t int_val;
    bool bool_val;
    char str_val[32];
    jettyd_value_type_t type;
    bool valid;                         /**< false if read failed */
} jettyd_value_t;

typedef struct {
    /* Identity */
    char instance[JETTYD_MAX_INSTANCE_NAME];  /**< e.g., "soil", "valve" */
    char driver_name[32];                      /**< e.g., "soil_moisture", "relay" */

    /* Capabilities this instance provides */
    jettyd_capability_t capabilities[JETTYD_MAX_CAPABILITIES];
    uint8_t capability_count;

    /* Lifecycle */
    esp_err_t (*init)(const void *config);     /**< Called once at boot */
    esp_err_t (*deinit)(void);                 /**< Called on shutdown */

    /* Operations */
    jettyd_value_t (*read)(const char *capability_name);
    esp_err_t (*write)(const char *capability_name, jettyd_value_t value);
    esp_err_t (*switch_on)(uint32_t duration_ms);   /**< 0 = indefinite */
    esp_err_t (*switch_off)(void);
    bool (*get_state)(void);                        /**< For switchable: is it on? */

    /* Optional */
    esp_err_t (*calibrate)(const char *type);       /**< e.g., "dry", "wet" */
    esp_err_t (*self_test)(void);                   /**< Returns OK if hardware responds */
} jettyd_driver_t;

/** Registration macro — called in each driver's register function */
#define JETTYD_REGISTER_DRIVER(driver_ptr) \
    jettyd_driver_registry_add(driver_ptr)

/* Registry functions */
esp_err_t jettyd_driver_registry_init(void);
esp_err_t jettyd_driver_registry_add(const jettyd_driver_t *driver);
const jettyd_driver_t *jettyd_driver_find(const char *instance);
const jettyd_driver_t *jettyd_driver_find_capability(const char *dotted_name);
uint8_t jettyd_driver_count(void);
const jettyd_driver_t *jettyd_driver_get(uint8_t index);

#endif /* JETTYD_DRIVER_H */
