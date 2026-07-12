/**
 * @file esp_psram.h
 * @brief Host-test stub for ESP-IDF PSRAM detection.
 */

#pragma once
#include <stdbool.h>

extern bool g_psram_available;

static inline bool esp_psram_is_initialized(void) { return g_psram_available; }
