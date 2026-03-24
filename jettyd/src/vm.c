/**
 * @file vm.c
 * @brief JettyScript rule VM — full implementation per spec section 6.
 *
 * Evaluates declarative rules and heartbeat schedules. All state is
 * in fixed-size arrays — no dynamic memory allocation after init.
 * Runs as a FreeRTOS task at medium priority with a 100ms tick.
 */

#include "jettyd_vm.h"
#include "jettyd_nvs.h"
#include "jettyd_mqtt.h"
#include "jettyd_telemetry.h"
#include "jettyd_shadow.h"
#include "jettyd.h"
#include "esp_log.h"
#include <stdbool.h>
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

static const char *TAG = "jettyd_vm";

/* ── cJSON arena allocator ─────────────────────────────────────────────────
 * All cJSON parsing uses a pre-allocated static arena, never the system heap.
 * Arena is reset at the start of each jettyd_vm_load_config() call.
 * Size: 8KB comfortably holds a config with 16 rules + 8 heartbeats.
 * ─────────────────────────────────────────────────────────────────────── */
#define JETTYD_VM_PARSE_ARENA_SIZE (8 * 1024)
static uint8_t  s_cjson_arena[JETTYD_VM_PARSE_ARENA_SIZE];
static size_t   s_cjson_arena_pos = 0;

static void *cjson_arena_malloc(size_t sz)
{
    sz = (sz + 7u) & ~7u; /* 8-byte align */
    if (s_cjson_arena_pos + sz > sizeof(s_cjson_arena)) {
        ESP_LOGE("vm_arena", "Arena exhausted (%u + %u > %u)",
                 (unsigned)s_cjson_arena_pos, (unsigned)sz,
                 (unsigned)sizeof(s_cjson_arena));
        return NULL;
    }
    void *p = &s_cjson_arena[s_cjson_arena_pos];
    s_cjson_arena_pos += sz;
    return p;
}

static void cjson_arena_free(void *p) { (void)p; /* arena: no individual frees */ }



static jettyd_vm_state_t s_vm = {0};
static TaskHandle_t s_vm_task = NULL;
static bool s_running = false;

/* Forward declarations */
static bool evaluate_condition(const jettyd_condition_t *cond);
static esp_err_t execute_action(const jettyd_action_t *action);
static esp_err_t parse_rules(cJSON *rules_arr, jettyd_vm_error_t *errors, uint8_t *err_count);
static esp_err_t parse_heartbeats(cJSON *hb_arr, jettyd_vm_error_t *errors, uint8_t *err_count);
static esp_err_t parse_condition(cJSON *obj, jettyd_condition_t *cond,
                                  jettyd_vm_error_t *errors, uint8_t *err_count,
                                  const char *rule_id, int depth);
static jettyd_op_t parse_op(const char *op_str);

/* ───────────────────────────── Init / Lifecycle ───────────────────────────── */

esp_err_t jettyd_vm_init(void)
{
    memset(&s_vm, 0, sizeof(s_vm));
    s_running = false;
    ESP_LOGI(TAG, "VM initialized");
    return ESP_OK;
}

const jettyd_vm_state_t *jettyd_vm_get_state(void)
{
    return &s_vm;
}

/* ───────────────────────────── Config Loading ─────────────────────────────── */

esp_err_t jettyd_vm_load_config(const char *json, int json_len,
                                 jettyd_vm_error_t *errors, uint8_t *error_count)
{
    if (json == NULL || json_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)json_len > JETTYD_VM_CONFIG_MAX_SIZE) {
        if (errors && error_count) {
            strncpy(errors[0].error, "Config exceeds 4KB limit", sizeof(errors[0].error) - 1);
            *error_count = 1;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Reset arena and install arena allocator hooks — no heap allocation */
    s_cjson_arena_pos = 0;
    cJSON_Hooks arena_hooks = { .malloc_fn = cjson_arena_malloc, .free_fn = cjson_arena_free };
    cJSON_InitHooks(&arena_hooks);
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL) {
        if (errors && error_count) {
            strncpy(errors[0].error, "Invalid JSON", sizeof(errors[0].error) - 1);
            *error_count = 1;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Version check */
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        cJSON_Delete(root);
        if (errors && error_count) {
            strncpy(errors[0].error, "Unsupported config version (expected 1)",
                    sizeof(errors[0].error) - 1);
            *error_count = 1;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Parse into temporary state so we can reject atomically */
    jettyd_vm_state_t new_state = {0};
    jettyd_vm_state_t old_state;
    memcpy(&old_state, &s_vm, sizeof(old_state));

    /* Temporarily swap in new state for parsing */
    memcpy(&s_vm, &new_state, sizeof(s_vm));

    uint8_t err_idx = 0;

    /* Parse rules */
    cJSON *rules_arr = cJSON_GetObjectItem(root, "rules");
    if (cJSON_IsArray(rules_arr)) {
        int count = cJSON_GetArraySize(rules_arr);
        if (count > JETTYD_VM_MAX_RULES) {
            if (errors) {
                snprintf(errors[err_idx].error, sizeof(errors[err_idx].error),
                         "Too many rules: %d (max %d)", count, JETTYD_VM_MAX_RULES);
                err_idx++;
            }
        } else {
            parse_rules(rules_arr, errors, &err_idx);
        }
    }

    /* Parse heartbeats */
    cJSON *hb_arr = cJSON_GetObjectItem(root, "heartbeats");
    if (cJSON_IsArray(hb_arr)) {
        int count = cJSON_GetArraySize(hb_arr);
        if (count > JETTYD_VM_MAX_HEARTBEATS) {
            if (errors) {
                snprintf(errors[err_idx].error, sizeof(errors[err_idx].error),
                         "Too many heartbeats: %d (max %d)", count, JETTYD_VM_MAX_HEARTBEATS);
                err_idx++;
            }
        } else {
            parse_heartbeats(hb_arr, errors, &err_idx);
        }
    }

    cJSON_Delete(root);
    cJSON_InitHooks(NULL); /* restore system malloc/free after parse */

    if (error_count) {
        *error_count = err_idx;
    }

    /* If any errors, reject and restore old state */
    if (err_idx > 0) {
        memcpy(&s_vm, &old_state, sizeof(s_vm));
        ESP_LOGW(TAG, "Config rejected with %d errors", err_idx);
        return ESP_ERR_INVALID_ARG;
    }

    /* Accept: keep new state */
    s_vm.loaded = true;
    ESP_LOGI(TAG, "Config loaded: %d rules, %d heartbeats",
             s_vm.rule_count, s_vm.heartbeat_count);

    /* Persist to NVS */
    jettyd_vm_persist_to_nvs();

    return ESP_OK;
}

static esp_err_t parse_rules(cJSON *rules_arr, jettyd_vm_error_t *errors, uint8_t *err_count)
{
    int count = cJSON_GetArraySize(rules_arr);

    for (int i = 0; i < count && i < JETTYD_VM_MAX_RULES; i++) {
        cJSON *rule_obj = cJSON_GetArrayItem(rules_arr, i);
        jettyd_rule_t *rule = &s_vm.rules[i];
        memset(rule, 0, sizeof(jettyd_rule_t));

        /* id */
        cJSON *id = cJSON_GetObjectItem(rule_obj, "id");
        if (cJSON_IsString(id)) {
            strncpy(rule->id, id->valuestring, sizeof(rule->id) - 1);
        } else {
            snprintf(rule->id, sizeof(rule->id), "r%d", i);
        }

        /* enabled (default true) */
        cJSON *enabled = cJSON_GetObjectItem(rule_obj, "enabled");
        rule->enabled = (enabled == NULL || cJSON_IsTrue(enabled));

        /* when (condition) */
        cJSON *when_obj = cJSON_GetObjectItem(rule_obj, "when");
        if (when_obj == NULL) {
            if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                strncpy(errors[*err_count].rule_id, rule->id, 7);
                strncpy(errors[*err_count].error, "Missing 'when' condition", 127);
                (*err_count)++;
            }
            continue;
        }
        parse_condition(when_obj, &rule->condition, errors, err_count, rule->id, 0);

        /* then (actions) */
        cJSON *then_arr = cJSON_GetObjectItem(rule_obj, "then");
        if (cJSON_IsArray(then_arr)) {
            int action_count = cJSON_GetArraySize(then_arr);
            if (action_count > JETTYD_VM_MAX_ACTIONS) {
                if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                    strncpy(errors[*err_count].rule_id, rule->id, 7);
                    snprintf(errors[*err_count].error, 127,
                             "Too many actions: %d (max %d)", action_count, JETTYD_VM_MAX_ACTIONS);
                    (*err_count)++;
                }
                action_count = JETTYD_VM_MAX_ACTIONS;
            }

            for (int a = 0; a < action_count; a++) {
                cJSON *action_obj = cJSON_GetArrayItem(then_arr, a);
                jettyd_action_t *act = &rule->actions[a];
                memset(act, 0, sizeof(jettyd_action_t));

                cJSON *action_type = cJSON_GetObjectItem(action_obj, "action");
                cJSON *target = cJSON_GetObjectItem(action_obj, "target");
                cJSON *params = cJSON_GetObjectItem(action_obj, "params");

                if (cJSON_IsString(target)) {
                    strncpy(act->target, target->valuestring, JETTYD_MAX_INSTANCE_NAME - 1);
                }

                if (!cJSON_IsString(action_type)) continue;
                const char *atype = action_type->valuestring;

                if (strcmp(atype, "switch_on") == 0) {
                    act->type = JETTYD_ACTION_SWITCH_ON;
                    /* Validate target is switchable */
                    if (act->target[0]) {
                        const jettyd_driver_t *drv = jettyd_driver_find(act->target);
                        if (drv == NULL) {
                            if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                                strncpy(errors[*err_count].rule_id, rule->id, 7);
                                snprintf(errors[*err_count].error, 127,
                                         "Unknown target: %s", act->target);
                                (*err_count)++;
                            }
                        }
                    }
                    if (params) {
                        cJSON *dur = cJSON_GetObjectItem(params, "duration");
                        if (cJSON_IsNumber(dur)) {
                            act->switch_on.duration_sec = (uint32_t)dur->valueint;
                        }
                    }
                } else if (strcmp(atype, "switch_off") == 0) {
                    act->type = JETTYD_ACTION_SWITCH_OFF;
                } else if (strcmp(atype, "set_value") == 0) {
                    act->type = JETTYD_ACTION_SET_VALUE;
                    if (params) {
                        cJSON *val = cJSON_GetObjectItem(params, "value");
                        if (cJSON_IsNumber(val)) {
                            act->set_value.value = (float)val->valuedouble;
                        }
                    }
                } else if (strcmp(atype, "report") == 0) {
                    act->type = JETTYD_ACTION_REPORT;
                    if (params) {
                        cJSON *metrics = cJSON_GetObjectItem(params, "metrics");
                        if (cJSON_IsArray(metrics)) {
                            int mc = cJSON_GetArraySize(metrics);
                            if (mc > JETTYD_VM_MAX_METRICS) mc = JETTYD_VM_MAX_METRICS;
                            for (int m = 0; m < mc; m++) {
                                cJSON *met = cJSON_GetArrayItem(metrics, m);
                                if (cJSON_IsString(met)) {
                                    strncpy(act->report.metrics[m], met->valuestring, 31);
                                }
                            }
                            act->report.metric_count = (uint8_t)mc;
                        }
                    }
                } else if (strcmp(atype, "alert") == 0) {
                    act->type = JETTYD_ACTION_ALERT;
                    if (params) {
                        cJSON *msg = cJSON_GetObjectItem(params, "message");
                        cJSON *sev = cJSON_GetObjectItem(params, "severity");
                        if (cJSON_IsString(msg)) {
                            strncpy(act->alert.message, msg->valuestring,
                                    sizeof(act->alert.message) - 1);
                        }
                        if (cJSON_IsString(sev)) {
                            strncpy(act->alert.severity, sev->valuestring,
                                    sizeof(act->alert.severity) - 1);
                        } else {
                            strncpy(act->alert.severity, "info", sizeof(act->alert.severity) - 1);
                        }
                    }
                } else if (strcmp(atype, "sleep") == 0) {
                    act->type = JETTYD_ACTION_SLEEP;
                    if (params) {
                        cJSON *sec = cJSON_GetObjectItem(params, "seconds");
                        if (cJSON_IsNumber(sec)) {
                            uint32_t s = (uint32_t)sec->valueint;
                            if (s > JETTYD_VM_MAX_SLEEP_SEC) s = JETTYD_VM_MAX_SLEEP_SEC;
                            act->sleep.seconds = s;
                        }
                    }
                } else if (strcmp(atype, "set_heartbeat") == 0) {
                    act->type = JETTYD_ACTION_SET_HEARTBEAT;
                    if (params) {
                        cJSON *intv = cJSON_GetObjectItem(params, "interval");
                        if (cJSON_IsNumber(intv)) {
                            uint32_t iv = (uint32_t)intv->valueint;
                            if (iv < JETTYD_VM_MIN_HEARTBEAT_SEC) iv = JETTYD_VM_MIN_HEARTBEAT_SEC;
                            if (iv > JETTYD_VM_MAX_HEARTBEAT_SEC) iv = JETTYD_VM_MAX_HEARTBEAT_SEC;
                            act->set_heartbeat.interval_sec = iv;
                        }
                    }
                }

                rule->action_count++;
            }
        }

        s_vm.rule_count++;
    }

    return ESP_OK;
}

static esp_err_t parse_condition(cJSON *obj, jettyd_condition_t *cond,
                                  jettyd_vm_error_t *errors, uint8_t *err_count,
                                  const char *rule_id, int depth)
{
    if (depth > JETTYD_VM_MAX_NESTING) {
        if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
            strncpy(errors[*err_count].rule_id, rule_id, 7);
            strncpy(errors[*err_count].error, "Condition nesting too deep (max 2)", 127);
            (*err_count)++;
        }
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *type_obj = cJSON_GetObjectItem(obj, "type");
    if (!cJSON_IsString(type_obj)) {
        if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
            strncpy(errors[*err_count].rule_id, rule_id, 7);
            strncpy(errors[*err_count].error, "Condition missing 'type'", 127);
            (*err_count)++;
        }
        return ESP_ERR_INVALID_ARG;
    }

    const char *type_str = type_obj->valuestring;

    if (strcmp(type_str, "threshold") == 0) {
        cond->type = JETTYD_COND_THRESHOLD;

        cJSON *sensor = cJSON_GetObjectItem(obj, "sensor");
        cJSON *op = cJSON_GetObjectItem(obj, "op");
        cJSON *value = cJSON_GetObjectItem(obj, "value");
        cJSON *debounce = cJSON_GetObjectItem(obj, "debounce");

        if (cJSON_IsString(sensor)) {
            strncpy(cond->threshold.sensor, sensor->valuestring,
                    sizeof(cond->threshold.sensor) - 1);

            /* Validate sensor exists in driver registry */
            if (jettyd_driver_find_capability(sensor->valuestring) == NULL &&
                strncmp(sensor->valuestring, "system.", 7) != 0) {
                if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                    strncpy(errors[*err_count].rule_id, rule_id, 7);
                    snprintf(errors[*err_count].error, 127,
                             "Unknown sensor: %s", sensor->valuestring);
                    (*err_count)++;
                }
            }
        }
        if (cJSON_IsString(op)) {
            cond->threshold.op = parse_op(op->valuestring);
        }
        if (cJSON_IsNumber(value)) {
            cond->threshold.value = (float)value->valuedouble;
        }
        if (cJSON_IsNumber(debounce)) {
            cond->threshold.debounce_sec = (uint32_t)debounce->valueint;
        }

    } else if (strcmp(type_str, "range") == 0) {
        cond->type = JETTYD_COND_RANGE;

        cJSON *sensor = cJSON_GetObjectItem(obj, "sensor");
        cJSON *min_val = cJSON_GetObjectItem(obj, "min");
        cJSON *max_val = cJSON_GetObjectItem(obj, "max");
        cJSON *on_enter = cJSON_GetObjectItem(obj, "on_enter");
        cJSON *on_exit = cJSON_GetObjectItem(obj, "on_exit");
        cJSON *debounce = cJSON_GetObjectItem(obj, "debounce");

        if (cJSON_IsString(sensor)) {
            strncpy(cond->range.sensor, sensor->valuestring,
                    sizeof(cond->range.sensor) - 1);
            if (jettyd_driver_find_capability(sensor->valuestring) == NULL &&
                strncmp(sensor->valuestring, "system.", 7) != 0) {
                if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                    strncpy(errors[*err_count].rule_id, rule_id, 7);
                    snprintf(errors[*err_count].error, 127,
                             "Unknown sensor: %s", sensor->valuestring);
                    (*err_count)++;
                }
            }
        }
        if (cJSON_IsNumber(min_val)) cond->range.min = (float)min_val->valuedouble;
        if (cJSON_IsNumber(max_val)) cond->range.max = (float)max_val->valuedouble;
        cond->range.on_enter = (on_enter == NULL || cJSON_IsTrue(on_enter));
        cond->range.on_exit = (on_exit != NULL && cJSON_IsTrue(on_exit));
        if (cJSON_IsNumber(debounce)) {
            cond->range.debounce_sec = (uint32_t)debounce->valueint;
        }

    } else if (strcmp(type_str, "compound") == 0) {
        cond->type = JETTYD_COND_COMPOUND;

        cJSON *operator_obj = cJSON_GetObjectItem(obj, "operator");
        cJSON *conditions = cJSON_GetObjectItem(obj, "conditions");

        if (cJSON_IsString(operator_obj)) {
            if (strcmp(operator_obj->valuestring, "and") == 0) {
                cond->compound.op = JETTYD_COMPOUND_AND;
            } else {
                cond->compound.op = JETTYD_COMPOUND_OR;
            }
        }

        if (cJSON_IsArray(conditions)) {
            int sub_count = cJSON_GetArraySize(conditions);
            if (sub_count > JETTYD_VM_MAX_CONDITIONS) sub_count = JETTYD_VM_MAX_CONDITIONS;

            if (s_vm.sub_cond_used + sub_count >
                JETTYD_VM_MAX_RULES * JETTYD_VM_MAX_CONDITIONS) {
                if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                    strncpy(errors[*err_count].rule_id, rule_id, 7);
                    strncpy(errors[*err_count].error,
                            "Sub-condition storage exhausted", 127);
                    (*err_count)++;
                }
                return ESP_ERR_NO_MEM;
            }

            cond->compound.sub_conditions = &s_vm.sub_cond_storage[s_vm.sub_cond_used];
            cond->compound.count = (uint8_t)sub_count;

            for (int i = 0; i < sub_count; i++) {
                cJSON *sub_obj = cJSON_GetArrayItem(conditions, i);
                parse_condition(sub_obj, &s_vm.sub_cond_storage[s_vm.sub_cond_used],
                               errors, err_count, rule_id, depth + 1);
                s_vm.sub_cond_used++;
            }
        }

    } else if (strcmp(type_str, "schedule") == 0) {
        cond->type = JETTYD_COND_SCHEDULE;

        cJSON *cron = cJSON_GetObjectItem(obj, "cron");
        if (cJSON_IsString(cron)) {
            /* Parse "HH:MM" or "HH:MM weekday_bitmask" */
            int hour = 0, minute = 0, bitmask = 0x7F; /* all days by default */
            if (sscanf(cron->valuestring, "%d:%d %d", &hour, &minute, &bitmask) < 2) {
                sscanf(cron->valuestring, "%d:%d", &hour, &minute);
            }
            cond->schedule.hour = (uint8_t)hour;
            cond->schedule.minute = (uint8_t)minute;
            cond->schedule.weekday_bitmask = (uint8_t)bitmask;
        }

    } else if (strcmp(type_str, "time_window") == 0) {
        cond->type = JETTYD_COND_TIME_WINDOW;

        cJSON *start = cJSON_GetObjectItem(obj, "start_hour");
        cJSON *end = cJSON_GetObjectItem(obj, "end_hour");
        cJSON *bitmask = cJSON_GetObjectItem(obj, "weekday_bitmask");

        if (cJSON_IsNumber(start)) cond->time_window.start_hour = (uint8_t)start->valueint;
        if (cJSON_IsNumber(end)) cond->time_window.end_hour = (uint8_t)end->valueint;
        if (cJSON_IsNumber(bitmask)) {
            cond->time_window.weekday_bitmask = (uint8_t)bitmask->valueint;
        } else {
            cond->time_window.weekday_bitmask = 0x7F;
        }
    } else {
        if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
            strncpy(errors[*err_count].rule_id, rule_id, 7);
            snprintf(errors[*err_count].error, 127,
                     "Unknown condition type: %s", type_str);
            (*err_count)++;
        }
    }

    return ESP_OK;
}

static esp_err_t parse_heartbeats(cJSON *hb_arr, jettyd_vm_error_t *errors, uint8_t *err_count)
{
    int count = cJSON_GetArraySize(hb_arr);

    for (int i = 0; i < count && i < JETTYD_VM_MAX_HEARTBEATS; i++) {
        cJSON *hb_obj = cJSON_GetArrayItem(hb_arr, i);
        jettyd_heartbeat_t *hb = &s_vm.heartbeats[i];
        memset(hb, 0, sizeof(jettyd_heartbeat_t));

        cJSON *id = cJSON_GetObjectItem(hb_obj, "id");
        if (cJSON_IsString(id)) {
            strncpy(hb->id, id->valuestring, sizeof(hb->id) - 1);
        } else {
            snprintf(hb->id, sizeof(hb->id), "h%d", i);
        }

        cJSON *every = cJSON_GetObjectItem(hb_obj, "every");
        if (cJSON_IsNumber(every)) {
            uint32_t iv = (uint32_t)every->valueint;
            if (iv < JETTYD_VM_MIN_HEARTBEAT_SEC) {
                if (errors && *err_count < JETTYD_VM_MAX_ERRORS) {
                    strncpy(errors[*err_count].rule_id, hb->id, 7);
                    snprintf(errors[*err_count].error, 127,
                             "Heartbeat interval %lu below minimum %d",
                             (unsigned long)iv, JETTYD_VM_MIN_HEARTBEAT_SEC);
                    (*err_count)++;
                }
                iv = JETTYD_VM_MIN_HEARTBEAT_SEC;
            }
            if (iv > JETTYD_VM_MAX_HEARTBEAT_SEC) {
                iv = JETTYD_VM_MAX_HEARTBEAT_SEC;
            }
            hb->interval_sec = iv;
        } else {
            hb->interval_sec = 60; /* Default */
        }

        cJSON *metrics = cJSON_GetObjectItem(hb_obj, "metrics");
        if (cJSON_IsArray(metrics)) {
            int mc = cJSON_GetArraySize(metrics);
            if (mc > JETTYD_VM_MAX_METRICS) mc = JETTYD_VM_MAX_METRICS;
            for (int m = 0; m < mc; m++) {
                cJSON *met = cJSON_GetArrayItem(metrics, m);
                if (cJSON_IsString(met)) {
                    strncpy(hb->metrics[m], met->valuestring, 31);
                }
            }
            hb->metric_count = (uint8_t)mc;
        }

        s_vm.heartbeat_count++;
    }

    return ESP_OK;
}

static jettyd_op_t parse_op(const char *op_str)
{
    if (strcmp(op_str, "<") == 0)  return JETTYD_OP_LT;
    if (strcmp(op_str, ">") == 0)  return JETTYD_OP_GT;
    if (strcmp(op_str, "<=") == 0) return JETTYD_OP_LTE;
    if (strcmp(op_str, ">=") == 0) return JETTYD_OP_GTE;
    if (strcmp(op_str, "==") == 0) return JETTYD_OP_EQ;
    if (strcmp(op_str, "!=") == 0) return JETTYD_OP_NEQ;
    return JETTYD_OP_EQ; /* Default */
}

/* ───────────────────────────── NVS Persistence ────────────────────────────── */

esp_err_t jettyd_vm_load_from_nvs(void)
{
    char buf[JETTYD_VM_CONFIG_MAX_SIZE];
    size_t len = sizeof(buf);
    esp_err_t err = jettyd_nvs_read_blob(JETTYD_NVS_NS_VM, "config", buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No config in NVS: %s", esp_err_to_name(err));
        return err;
    }

    jettyd_vm_error_t errors[JETTYD_VM_MAX_ERRORS];
    uint8_t error_count = 0;
    err = jettyd_vm_load_config(buf, (int)len, errors, &error_count);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS config invalid, entering failsafe");
        jettyd_vm_enter_failsafe("config_parse_error");
    }

    return err;
}

esp_err_t jettyd_vm_persist_to_nvs(void)
{
    /* Serialize VM config to NVS using static buffer — no heap allocation.
     * Buffer size: 4KB accommodates up to 16 rules + 8 heartbeats with metadata. */
    static char config_buf[4096];
    int pos = 0;
    int rem = (int)sizeof(config_buf);

#define APPEND(...) do { int n = snprintf(config_buf + pos, rem, __VA_ARGS__); if (n < 0 || n >= rem) return ESP_ERR_NO_MEM; pos += n; rem -= n; } while(0)

    APPEND("{\"version\":1,\"rules\":[");
    for (uint8_t i = 0; i < s_vm.rule_count; i++) {
        if (i > 0) APPEND(",");
        APPEND("{\"id\":\"%s\",\"enabled\":%s}",
               s_vm.rules[i].id,
               s_vm.rules[i].enabled ? "true" : "false");
    }
    APPEND("],\"heartbeats\":[");
    for (uint8_t i = 0; i < s_vm.heartbeat_count; i++) {
        if (i > 0) APPEND(",");
        APPEND("{\"id\":\"%s\",\"every\":%lu,\"metrics\":[",
               s_vm.heartbeats[i].id,
               (unsigned long)s_vm.heartbeats[i].interval_sec);
        for (uint8_t m = 0; m < s_vm.heartbeats[i].metric_count; m++) {
            if (m > 0) APPEND(",");
            APPEND("\"%s\"", s_vm.heartbeats[i].metrics[m]);
        }
        APPEND("]}");
    }
    APPEND("]}");

#undef APPEND

    return jettyd_nvs_write_blob(JETTYD_NVS_NS_VM, "config", config_buf, (size_t)pos);
}

/* ───────────────────────────── Evaluation Engine ──────────────────────────── */

/**
 * @brief Read a sensor value by dotted name (e.g., "soil.moisture").
 */
static float read_sensor_float(const char *dotted_name)
{
    const jettyd_driver_t *drv = jettyd_driver_find_capability(dotted_name);
    if (drv == NULL || drv->read == NULL) {
        return 0.0f;
    }
    const char *dot = strchr(dotted_name, '.');
    if (dot == NULL) return 0.0f;

    jettyd_value_t val = drv->read(dot + 1);
    if (!val.valid) return 0.0f;

    switch (val.type) {
    case JETTYD_VAL_FLOAT: return val.float_val;
    case JETTYD_VAL_INT:   return (float)val.int_val;
    case JETTYD_VAL_BOOL:  return val.bool_val ? 1.0f : 0.0f;
    default:               return 0.0f;
    }
}

static bool compare_float(float lhs, jettyd_op_t op, float rhs)
{
    switch (op) {
    case JETTYD_OP_LT:  return lhs < rhs;
    case JETTYD_OP_GT:  return lhs > rhs;
    case JETTYD_OP_LTE: return lhs <= rhs;
    case JETTYD_OP_GTE: return lhs >= rhs;
    case JETTYD_OP_EQ:  return fabsf(lhs - rhs) < 0.001f;
    case JETTYD_OP_NEQ: return fabsf(lhs - rhs) >= 0.001f;
    }
    return false;
}

static bool evaluate_condition(const jettyd_condition_t *cond)
{
    switch (cond->type) {
    case JETTYD_COND_THRESHOLD: {
        float val = read_sensor_float(cond->threshold.sensor);
        return compare_float(val, cond->threshold.op, cond->threshold.value);
    }

    case JETTYD_COND_RANGE: {
        float val = read_sensor_float(cond->range.sensor);
        bool in_range = (val >= cond->range.min && val <= cond->range.max);
        /* The caller handles edge detection; here we just return current state */
        return in_range;
    }

    case JETTYD_COND_COMPOUND: {
        if (cond->compound.count == 0) return false;

        if (cond->compound.op == JETTYD_COMPOUND_AND) {
            for (uint8_t i = 0; i < cond->compound.count; i++) {
                if (!evaluate_condition(&cond->compound.sub_conditions[i])) {
                    return false;
                }
            }
            return true;
        } else { /* OR */
            for (uint8_t i = 0; i < cond->compound.count; i++) {
                if (evaluate_condition(&cond->compound.sub_conditions[i])) {
                    return true;
                }
            }
            return false;
        }
    }

    case JETTYD_COND_SCHEDULE: {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        if (tm_now.tm_hour == cond->schedule.hour &&
            tm_now.tm_min == cond->schedule.minute) {
            /* Check weekday: tm_wday: 0=Sun..6=Sat; bitmask: bit0=Mon..bit6=Sun */
            int day_bit = (tm_now.tm_wday == 0) ? 6 : (tm_now.tm_wday - 1);
            return (cond->schedule.weekday_bitmask & (1 << day_bit)) != 0;
        }
        return false;
    }

    case JETTYD_COND_TIME_WINDOW: {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        int day_bit = (tm_now.tm_wday == 0) ? 6 : (tm_now.tm_wday - 1);
        if (!(cond->time_window.weekday_bitmask & (1 << day_bit))) {
            return false;
        }

        uint8_t hour = (uint8_t)tm_now.tm_hour;
        if (cond->time_window.start_hour <= cond->time_window.end_hour) {
            return hour >= cond->time_window.start_hour &&
                   hour < cond->time_window.end_hour;
        } else {
            /* Wraps around midnight */
            return hour >= cond->time_window.start_hour ||
                   hour < cond->time_window.end_hour;
        }
    }
    }

    return false;
}

static esp_err_t execute_action(const jettyd_action_t *action)
{
    switch (action->type) {
    case JETTYD_ACTION_SWITCH_ON: {
        const jettyd_driver_t *drv = jettyd_driver_find(action->target);
        if (drv == NULL || drv->switch_on == NULL) {
            ESP_LOGW(TAG, "Cannot switch_on: driver '%s' not found or not switchable",
                     action->target);
            return ESP_ERR_NOT_FOUND;
        }
        uint32_t dur_ms = action->switch_on.duration_sec * 1000;
        ESP_LOGI(TAG, "Action: switch_on %s (duration: %lu ms)",
                 action->target, (unsigned long)dur_ms);
        return drv->switch_on(dur_ms);
    }

    case JETTYD_ACTION_SWITCH_OFF: {
        const jettyd_driver_t *drv = jettyd_driver_find(action->target);
        if (drv == NULL || drv->switch_off == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        ESP_LOGI(TAG, "Action: switch_off %s", action->target);
        return drv->switch_off();
    }

    case JETTYD_ACTION_SET_VALUE: {
        const jettyd_driver_t *drv = jettyd_driver_find(action->target);
        if (drv == NULL || drv->write == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        jettyd_value_t val = {
            .type = JETTYD_VAL_FLOAT,
            .float_val = action->set_value.value,
            .valid = true
        };
        ESP_LOGI(TAG, "Action: set_value %s = %.2f", action->target, val.float_val);
        return drv->write(NULL, val);
    }

    case JETTYD_ACTION_REPORT: {
        if (action->report.metric_count == 0) {
            return jettyd_telemetry_publish_all();
        }
        const char *metrics[JETTYD_VM_MAX_METRICS];
        for (uint8_t i = 0; i < action->report.metric_count; i++) {
            metrics[i] = action->report.metrics[i];
        }
        return jettyd_telemetry_publish(metrics, action->report.metric_count);
    }

    case JETTYD_ACTION_ALERT: {
        char resolved[256];
        jettyd_vm_template_subst(action->alert.message, resolved, sizeof(resolved));
        ESP_LOGI(TAG, "Action: alert [%s] %s", action->alert.severity, resolved);
        return jettyd_telemetry_publish_alert(resolved, action->alert.severity);
    }

    case JETTYD_ACTION_SLEEP: {
        uint32_t sec = action->sleep.seconds;
        if (sec > JETTYD_VM_MAX_SLEEP_SEC) sec = JETTYD_VM_MAX_SLEEP_SEC;
        ESP_LOGD(TAG, "Action: sleep %lu sec", (unsigned long)sec);
        vTaskDelay(pdMS_TO_TICKS(sec * 1000));
        return ESP_OK;
    }

    case JETTYD_ACTION_SET_HEARTBEAT: {
        /* Modify the first heartbeat's interval */
        if (s_vm.heartbeat_count > 0) {
            s_vm.heartbeats[0].interval_sec = action->set_heartbeat.interval_sec;
            ESP_LOGI(TAG, "Action: set_heartbeat %lu sec",
                     (unsigned long)action->set_heartbeat.interval_sec);
        }
        return ESP_OK;
    }
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t jettyd_vm_tick(void)
{
    int64_t now_us = esp_timer_get_time();

    /* ── Heartbeats ── */
    for (uint8_t i = 0; i < s_vm.heartbeat_count; i++) {
        jettyd_heartbeat_t *hb = &s_vm.heartbeats[i];
        int64_t elapsed_us = now_us - hb->last_fired_us;
        int64_t interval_us = (int64_t)hb->interval_sec * 1000000LL;

        if (elapsed_us >= interval_us) {
            const char *metrics[JETTYD_VM_MAX_METRICS];
            for (uint8_t m = 0; m < hb->metric_count; m++) {
                metrics[m] = hb->metrics[m];
            }
            jettyd_telemetry_publish(metrics, hb->metric_count);
            hb->last_fired_us = now_us;
        }
    }

    /* ── Rules ── */
    for (uint8_t i = 0; i < s_vm.rule_count; i++) {
        jettyd_rule_t *rule = &s_vm.rules[i];
        if (!rule->enabled) continue;

        bool current = evaluate_condition(&rule->condition);

        /* Edge detection: only fire on FALSE→TRUE crossing */
        if (current && !rule->last_condition_state) {
            /* Debounce check */
            uint32_t debounce_sec = 0;
            if (rule->condition.type == JETTYD_COND_THRESHOLD) {
                debounce_sec = rule->condition.threshold.debounce_sec;
            } else if (rule->condition.type == JETTYD_COND_RANGE) {
                debounce_sec = rule->condition.range.debounce_sec;
            }

            int64_t debounce_us = (int64_t)debounce_sec * 1000000LL;
            if (now_us - rule->last_triggered_us >= debounce_us) {
                ESP_LOGI(TAG, "Rule %s triggered", rule->id);
                for (uint8_t a = 0; a < rule->action_count; a++) {
                    execute_action(&rule->actions[a]);
                }
                rule->last_triggered_us = now_us;
            }
        }

        rule->last_condition_state = current;
    }

    return ESP_OK;
}

/* ───────────────────────────── FreeRTOS Task ──────────────────────────────── */

static void vm_task(void *arg)
{
    ESP_LOGI(TAG, "VM task started (tick: %d ms)", JETTYD_VM_TICK_MS);

    while (s_running) {
        jettyd_vm_tick();
        vTaskDelay(pdMS_TO_TICKS(JETTYD_VM_TICK_MS));
    }

    ESP_LOGI(TAG, "VM task stopped");
    vTaskDelete(NULL);
}

esp_err_t jettyd_vm_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    s_running = true;
    BaseType_t ret = xTaskCreate(vm_task, "jettyd_vm", 8192, NULL, 5, &s_vm_task);
    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create VM task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t jettyd_vm_stop(void)
{
    s_running = false;
    /* Task will delete itself on next tick */
    s_vm_task = NULL;
    return ESP_OK;
}

/* ───────────────────────────── MQTT Handler ───────────────────────────────── */

void jettyd_vm_config_handler(const char *topic, const char *data, int data_len)
{
    ESP_LOGI(TAG, "Received config update (%d bytes)", data_len);

    jettyd_vm_error_t errors[JETTYD_VM_MAX_ERRORS];
    uint8_t error_count = 0;

    esp_err_t err = jettyd_vm_load_config(data, data_len, errors, &error_count);

    /* Publish ack or rejection */
    char topic_buf[JETTYD_MQTT_MAX_TOPIC];

    if (err == ESP_OK) {
        /* ACK */
        if (jettyd_mqtt_build_topic(topic_buf, sizeof(topic_buf), "command/response") == ESP_OK) {
            char ack[64];
            snprintf(ack, sizeof(ack), "{\"type\":\"config_accepted\",\"rules\":%d,\"heartbeats\":%d}",
                     s_vm.rule_count, s_vm.heartbeat_count);
            jettyd_mqtt_publish(topic_buf, ack, 1, false);
        }
    } else {
        /* Rejection with error details — static buffer, no heap allocation */
        static char reject_buf[1024];
        int rpos = 0;
        int rrem = (int)sizeof(reject_buf);
#define RAPP(...) do { int n = snprintf(reject_buf + rpos, rrem, __VA_ARGS__); if (n > 0 && n < rrem) { rpos += n; rrem -= n; } } while(0)
        RAPP("{\"type\":\"config_rejected\",\"errors\":[");
        for (uint8_t i = 0; i < error_count; i++) {
            if (i > 0) RAPP(",");
            if (errors[i].rule_id[0]) {
                RAPP("{\"rule\":\"%s\",\"error\":\"%s\"}", errors[i].rule_id, errors[i].error);
            } else {
                RAPP("{\"error\":\"%s\"}", errors[i].error);
            }
        }
        RAPP("]}");
#undef RAPP
        if (jettyd_mqtt_build_topic(topic_buf, sizeof(topic_buf), "alert") == ESP_OK) {
            jettyd_mqtt_publish(topic_buf, reject_buf, 1, false);
        }
    }
}

/* ───────────────────────────── Fail-safe Mode ─────────────────────────────── */

esp_err_t jettyd_vm_enter_failsafe(const char *reason)
{
    ESP_LOGW(TAG, "Entering fail-safe mode: %s", reason ? reason : "unknown");

    /* Clear all rules */
    memset(s_vm.rules, 0, sizeof(s_vm.rules));
    s_vm.rule_count = 0;

    /* Switch off all actuators */
    uint8_t drv_count = jettyd_driver_count();
    for (uint8_t i = 0; i < drv_count; i++) {
        const jettyd_driver_t *drv = jettyd_driver_get(i);
        if (drv && drv->switch_off) {
            drv->switch_off();
        }
    }

    /* Keep default heartbeat if we have one, otherwise set a basic one */
    if (s_vm.heartbeat_count == 0) {
        const jettyd_config_t *cfg = jettyd_get_config();
        s_vm.heartbeat_count = 1;
        strncpy(s_vm.heartbeats[0].id, "default", sizeof(s_vm.heartbeats[0].id) - 1);
        s_vm.heartbeats[0].interval_sec = cfg ? cfg->heartbeat_interval_sec : 60;
        s_vm.heartbeats[0].metric_count = 0; /* Report all */
    }

    s_vm.loaded = true;

    /* Publish alert */
    char alert_json[128];
    snprintf(alert_json, sizeof(alert_json),
             "{\"type\":\"failsafe\",\"reason\":\"%s\"}", reason ? reason : "unknown");

    char topic[JETTYD_MQTT_MAX_TOPIC];
    if (jettyd_mqtt_build_topic(topic, sizeof(topic), "alert") == ESP_OK) {
        jettyd_mqtt_publish(topic, alert_json, 1, false);
    }

    return ESP_OK;
}

/* ───────────────────────────── Template Substitution ──────────────────────── */

esp_err_t jettyd_vm_template_subst(const char *tmpl, char *out, size_t out_len)
{
    if (tmpl == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t out_pos = 0;
    const char *p = tmpl;

    while (*p && out_pos < out_len - 1) {
        if (p[0] == '{' && p[1] == '{') {
            /* Find closing }} */
            const char *end = strstr(p + 2, "}}");
            if (end == NULL) {
                /* No closing — copy literally */
                out[out_pos++] = *p++;
                continue;
            }

            /* Extract variable name */
            size_t var_len = (size_t)(end - p - 2);
            char var_name[48];
            if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
            memcpy(var_name, p + 2, var_len);
            var_name[var_len] = '\0';

            /* Read current value */
            float val = read_sensor_float(var_name);
            int written = snprintf(out + out_pos, out_len - out_pos, "%.1f", val);
            if (written > 0) out_pos += (size_t)written;

            p = end + 2;
        } else {
            out[out_pos++] = *p++;
        }
    }

    out[out_pos] = '\0';
    return ESP_OK;
}
