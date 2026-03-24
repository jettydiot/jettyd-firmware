/**
 * @file jettyd_vm.h
 * @brief JettyScript rule VM interface for the Jettyd firmware SDK.
 *
 * The VM evaluates declarative rules and heartbeat schedules received
 * via MQTT. No dynamic memory allocation — all state is in fixed arrays.
 */

#ifndef JETTYD_VM_H
#define JETTYD_VM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jettyd_driver.h"

/** VM limits (from spec section 8) */
#define JETTYD_VM_MAX_RULES         16
#define JETTYD_VM_MAX_HEARTBEATS    8
#define JETTYD_VM_MAX_ACTIONS       4
#define JETTYD_VM_MAX_METRICS       8
#define JETTYD_VM_MAX_CONDITIONS    4   /**< For compound conditions */
#define JETTYD_VM_MAX_NESTING       2   /**< Max compound nesting depth */
#define JETTYD_VM_CONFIG_MAX_SIZE   2048 /**< Max serialized config size */
#define JETTYD_VM_TICK_MS           100  /**< Evaluation loop interval */
#define JETTYD_VM_MIN_HEARTBEAT_SEC 10
#define JETTYD_VM_MAX_HEARTBEAT_SEC 3600
#define JETTYD_VM_MAX_SLEEP_SEC     60

/** Condition types */
typedef enum {
    JETTYD_COND_THRESHOLD,
    JETTYD_COND_RANGE,
    JETTYD_COND_COMPOUND,
    JETTYD_COND_SCHEDULE,
    JETTYD_COND_TIME_WINDOW,
} jettyd_condition_type_t;

/** Comparison operators */
typedef enum {
    JETTYD_OP_LT,       /**< < */
    JETTYD_OP_GT,       /**< > */
    JETTYD_OP_LTE,      /**< <= */
    JETTYD_OP_GTE,      /**< >= */
    JETTYD_OP_EQ,       /**< == */
    JETTYD_OP_NEQ,      /**< != */
} jettyd_op_t;

/** Compound operators */
typedef enum {
    JETTYD_COMPOUND_AND,
    JETTYD_COMPOUND_OR,
} jettyd_compound_op_t;

/** Action types */
typedef enum {
    JETTYD_ACTION_SWITCH_ON,
    JETTYD_ACTION_SWITCH_OFF,
    JETTYD_ACTION_SET_VALUE,
    JETTYD_ACTION_REPORT,
    JETTYD_ACTION_ALERT,
    JETTYD_ACTION_SLEEP,
    JETTYD_ACTION_SET_HEARTBEAT,
} jettyd_action_type_t;

/** Forward declaration for compound conditions */
typedef struct jettyd_condition jettyd_condition_t;

/** Condition definition */
struct jettyd_condition {
    jettyd_condition_type_t type;

    union {
        struct {
            char sensor[48];        /**< e.g., "soil.moisture" */
            jettyd_op_t op;
            float value;
            uint32_t debounce_sec;
        } threshold;

        struct {
            char sensor[48];
            float min;
            float max;
            bool on_enter;
            bool on_exit;
            uint32_t debounce_sec;
        } range;

        struct {
            jettyd_compound_op_t op;
            jettyd_condition_t *sub_conditions;  /**< Points into sub_cond_storage */
            uint8_t count;
        } compound;

        struct {
            uint8_t hour;
            uint8_t minute;
            uint8_t weekday_bitmask; /**< bit 0 = Mon, bit 6 = Sun */
        } schedule;

        struct {
            uint8_t start_hour;
            uint8_t end_hour;
            uint8_t weekday_bitmask;
        } time_window;
    };
};

/** Action definition */
typedef struct {
    jettyd_action_type_t type;
    char target[JETTYD_MAX_INSTANCE_NAME];  /**< Driver instance for switch/set */

    union {
        struct {
            uint32_t duration_sec;  /**< Auto-off after N seconds, 0 = indefinite */
        } switch_on;

        struct {
            float value;
        } set_value;

        struct {
            char metrics[JETTYD_VM_MAX_METRICS][32];
            uint8_t metric_count;   /**< 0 = report all */
        } report;

        struct {
            char message[128];
            char severity[12];      /**< "info", "warning", "critical" */
        } alert;

        struct {
            uint32_t seconds;       /**< Max 60 */
        } sleep;

        struct {
            uint32_t interval_sec;  /**< 10–3600 */
        } set_heartbeat;
    };
} jettyd_action_t;

/** Rule definition (fixed-size, no heap allocation) */
typedef struct {
    char id[8];
    bool enabled;
    jettyd_condition_t condition;
    jettyd_action_t actions[JETTYD_VM_MAX_ACTIONS];
    uint8_t action_count;
    int64_t last_triggered_us;          /**< For debounce */
    bool last_condition_state;          /**< For edge detection */
} jettyd_rule_t;

/** Heartbeat definition */
typedef struct {
    char id[8];
    uint32_t interval_sec;
    char metrics[JETTYD_VM_MAX_METRICS][32];
    uint8_t metric_count;
    int64_t last_fired_us;
} jettyd_heartbeat_t;

/** Full VM state — all statically allocated */
typedef struct {
    jettyd_rule_t rules[JETTYD_VM_MAX_RULES];
    uint8_t rule_count;
    jettyd_heartbeat_t heartbeats[JETTYD_VM_MAX_HEARTBEATS];
    uint8_t heartbeat_count;
    bool loaded;

    /* Storage for compound sub-conditions (avoids dynamic allocation) */
    jettyd_condition_t sub_cond_storage[JETTYD_VM_MAX_RULES * JETTYD_VM_MAX_CONDITIONS];
    uint8_t sub_cond_used;
} jettyd_vm_state_t;

/** Config validation error */
typedef struct {
    char rule_id[8];
    char error[128];
} jettyd_vm_error_t;

#define JETTYD_VM_MAX_ERRORS 16

/**
 * @brief Initialize the VM.
 *
 * Zeros the VM state. Does not load rules.
 */
esp_err_t jettyd_vm_init(void);

/**
 * @brief Load a JettyScript config from a JSON string.
 *
 * Validates all rules against the driver registry. On any validation
 * failure, the entire config is rejected and the previous config remains.
 *
 * @param json       Config JSON string.
 * @param json_len   JSON length.
 * @param errors     Output: validation errors (caller-allocated).
 * @param error_count Output: number of errors.
 * @return ESP_OK if loaded, ESP_ERR_INVALID_ARG if validation failed.
 */
esp_err_t jettyd_vm_load_config(const char *json, int json_len,
                                 jettyd_vm_error_t *errors, uint8_t *error_count);

/**
 * @brief Load the JettyScript config from NVS.
 *
 * Called at boot. If NVS is empty or corrupt, returns ESP_ERR_NOT_FOUND.
 */
esp_err_t jettyd_vm_load_from_nvs(void);

/**
 * @brief Persist the current config to NVS.
 */
esp_err_t jettyd_vm_persist_to_nvs(void);

/**
 * @brief Start the VM FreeRTOS task.
 *
 * The task runs the evaluation loop every JETTYD_VM_TICK_MS.
 */
esp_err_t jettyd_vm_start(void);

/**
 * @brief Stop the VM task.
 */
esp_err_t jettyd_vm_stop(void);

/**
 * @brief Run one evaluation tick (for testing).
 *
 * Evaluates all heartbeats and rules once.
 */
esp_err_t jettyd_vm_tick(void);

/**
 * @brief Get the current VM state (read-only).
 */
const jettyd_vm_state_t *jettyd_vm_get_state(void);

/**
 * @brief MQTT callback for the config topic.
 */
void jettyd_vm_config_handler(const char *topic, const char *data, int data_len);

/**
 * @brief Enter fail-safe mode.
 *
 * Clears all rules, sets all actuators to default state,
 * and runs heartbeat-only with the default interval.
 */
esp_err_t jettyd_vm_enter_failsafe(const char *reason);

/**
 * @brief Perform template substitution in an alert message.
 *
 * Replaces {{instance.capability}} with current readings.
 *
 * @param tmpl   Template string.
 * @param out    Output buffer.
 * @param out_len Buffer size.
 */
esp_err_t jettyd_vm_template_subst(const char *tmpl, char *out, size_t out_len);

#endif /* JETTYD_VM_H */
