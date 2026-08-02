/**
 * @file jettyd_wifi_portal.h
 * @brief SoftAP fallback config portal for WiFi recovery (FLU-164).
 *
 * When station WiFi cannot connect after a configurable number of consecutive
 * attempts and the portal is armed (fresh device, boot window, or button held),
 * a SoftAP + embedded HTTP server lets the user enter new credentials.
 *
 * Pure decision/validation helpers are always compiled and host-testable.
 * The SoftAP/HTTP bring-up is guarded by CONFIG_JETTYD_WIFI_PORTAL and
 * CONFIG_SOC_WIFI_SUPPORTED so disabled builds carry zero code or data.
 */

#ifndef JETTYD_WIFI_PORTAL_H
#define JETTYD_WIFI_PORTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** 802.11 credential limits (same as wifi.c) */
#define JETTYD_PORTAL_SSID_MAX 32
#define JETTYD_PORTAL_PASS_MAX 64

/* ── Pure host-testable helpers ─────────────────────────────────────────── */

/**
 * @brief Return true if the portal should start.
 *
 * Both conditions must hold: fail_count has reached the threshold AND the
 * portal is armed. A jammer driving disconnects cannot summon the portal on
 * a long-running device that is past its boot window and has its button released.
 */
bool jettyd_portal_should_trigger(int fail_count, int threshold, bool armed);

/**
 * @brief Return true if the portal is armed (any of three conditions).
 *
 * @param no_ssid         True when no SSID is stored in NVS (fresh device).
 * @param in_boot_window  True when elapsed boot time < BOOT_WINDOW_S.
 * @param button_held_3s  True when the boot/user button is held ≥3 s.
 */
bool jettyd_portal_is_armed(bool no_ssid, bool in_boot_window, bool button_held_3s);

/**
 * @brief Validate WiFi credential lengths per 802.11 limits.
 *
 * @return ESP_OK           Credentials are valid.
 * @return ESP_ERR_INVALID_ARG  ssid is NULL.
 * @return ESP_ERR_INVALID_SIZE ssid is empty or >32 bytes; password >64 bytes.
 */
esp_err_t jettyd_portal_validate_creds(const char *ssid, const char *password);

/**
 * @brief Parse a URL-encoded POST body and extract SSID + password.
 *
 * Supports %XX URL encoding and '+' as space. Validates lengths.
 *
 * @param body      POST body (need not be NUL-terminated).
 * @param body_len  Byte count of body.
 * @param ssid_out  Output buffer — JETTYD_PORTAL_SSID_MAX+1 bytes.
 * @param pass_out  Output buffer — JETTYD_PORTAL_PASS_MAX+1 bytes.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE on failure.
 */
esp_err_t jettyd_portal_parse_post_body(const char *body, size_t body_len,
                                        char *ssid_out, char *pass_out);

/**
 * @brief Build the SoftAP SSID "jettyd-<last6>" from device_id.
 *
 * Falls back to "jettyd-000000" when device_id is NULL or empty (i.e. before
 * provisioning, where the MAC suffix would normally be used on-target).
 */
void jettyd_portal_make_ap_ssid(const char *device_id, char *out, size_t out_len);

/**
 * @brief Return true when idle time since last_activity_us exceeds timeout_s.
 *
 * @param last_activity_us  Timestamp from esp_timer_get_time() (microseconds).
 * @param timeout_s         Idle timeout in seconds.
 */
bool jettyd_portal_is_timed_out(int64_t last_activity_us, uint32_t timeout_s);

/* ── Testable credential-persist helper ─────────────────────────────────── */

#if CONFIG_JETTYD_WIFI_PORTAL || defined(JETTYD_PORTAL_HOST_TEST)
/**
 * @brief Validate credentials and persist them to the provisioning NVS keys.
 *
 * Writes only wifi_ssid and wifi_pass in namespace jettyd_prov.
 * The identity keys (device_key, fleet_token, tenant_id) are never touched.
 *
 * @return ESP_OK on success, or a validation / NVS error.
 */
esp_err_t jettyd_portal_save_creds(const char *ssid, const char *password);
#endif

/* ── On-target portal start/stop ────────────────────────────────────────── */

#if CONFIG_JETTYD_WIFI_PORTAL && CONFIG_SOC_WIFI_SUPPORTED
/**
 * @brief Start the SoftAP config portal and block until done.
 *
 * Scans visible networks (in STA mode) before switching to AP, then starts
 * an HTTP server on 192.168.4.1. Blocks until the user saves credentials
 * (NVS write + reboot) or the idle timeout elapses (reboot to retry station).
 * This function never returns — it always ends in esp_restart().
 *
 * @param device_id  Device ID for the AP SSID suffix; may be "" if unprovisioned.
 */
esp_err_t jettyd_portal_start(const char *device_id);
#endif

#endif /* JETTYD_WIFI_PORTAL_H */
