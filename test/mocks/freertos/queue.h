/**
 * @file queue.h
 * @brief Host-test mock for FreeRTOS queues.
 *
 * A minimal, generic fixed-capacity ring buffer keyed by a QueueHandle_t.
 * xQueueReceive is non-blocking here (returns pdFALSE when empty) so tests can
 * "pump" the camera upload queue synchronously; the real FreeRTOS queue blocks
 * on portMAX_DELAY, which the dedicated upload task relies on on-device.
 */

#pragma once
#include "../esp_idf_stubs.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    unsigned char *storage;
    size_t         item_size;
    size_t         capacity;
    size_t         count;
    size_t         head;
} mock_queue_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    mock_queue_t *q = (mock_queue_t *)calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->storage   = (unsigned char *)calloc(length ? length : 1,
                                           item_size ? item_size : 1);
    q->item_size = item_size;
    q->capacity  = length;
    q->count     = 0;
    q->head      = 0;
    return (QueueHandle_t)q;
}

static inline BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t wait)
{
    (void)wait;
    mock_queue_t *q = (mock_queue_t *)handle;
    if (!q || q->count >= q->capacity) return pdFALSE;
    size_t idx = (q->head + q->count) % q->capacity;
    memcpy(q->storage + idx * q->item_size, item, q->item_size);
    q->count++;
    return pdPASS;
}

static inline BaseType_t xQueueReceive(QueueHandle_t handle, void *buf, TickType_t wait)
{
    (void)wait;
    mock_queue_t *q = (mock_queue_t *)handle;
    if (!q || q->count == 0) return pdFALSE;
    memcpy(buf, q->storage + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return pdPASS;
}
