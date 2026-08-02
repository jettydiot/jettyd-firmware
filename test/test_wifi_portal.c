/**
 * @file test_wifi_portal.c
 * @brief Host unit tests for the SoftAP fallback config portal (FLU-164).
 *
 * White-box pattern: wifi_portal.c is compiled directly with
 * JETTYD_PORTAL_HOST_TEST so the pure host-testable functions are exercised
 * without requiring ESP-IDF hardware abstractions.  The NVS seam is supplied
 * by the fake below; identity keys (device_key / fleet_token) are pre-seeded
 * and asserted untouched after every credential-save call.
 *
 * Coverage (per AC):
 *  Trigger state machine
 *    - below threshold → no portal
 *    - threshold reached but not armed → no portal
 *    - arming: fresh device (no SSID) → armed
 *    - arming: in boot window → armed
 *    - arming: button held ≥3 s → armed
 *  POST /save credential validation
 *    - valid creds accepted and written to NVS
 *    - empty SSID → rejected
 *    - SSID > 32 bytes → rejected
 *    - password > 64 bytes → rejected
 *  NVS key safety
 *    - only wifi_ssid / wifi_pass are written
 *    - fleet_token / device_key are never written
 *  POST body parsing
 *    - basic ssid=X&password=Y
 *    - absent password field treated as empty string
 *    - URL-encoded characters decoded
 *  SSID suffix builder
 *    - produces "jettyd-<last6>" from device_id
 *    - falls back gracefully when device_id is empty
 *  Idle timeout
 *    - past last_activity → timed out
 *    - recent last_activity → not timed out
 */

#include "mocks/esp_idf_stubs.h"

/* ── Fake NVS (jettyd_prov namespace only) ────────────────────────────── */

#define FAKE_NVS_MAX 16

typedef struct {
    char key[32];
    char val[129];
    bool present;
    int  writes;
} fake_nvs_entry_t;

static fake_nvs_entry_t s_nvs[FAKE_NVS_MAX];

static fake_nvs_entry_t *nvs_find(const char *key)
{
    for (int i = 0; i < FAKE_NVS_MAX; i++) {
        if (s_nvs[i].present && strcmp(s_nvs[i].key, key) == 0)
            return &s_nvs[i];
    }
    return NULL;
}

static fake_nvs_entry_t *nvs_slot(const char *key)
{
    fake_nvs_entry_t *e = nvs_find(key);
    if (e) return e;
    for (int i = 0; i < FAKE_NVS_MAX; i++) {
        if (!s_nvs[i].present) {
            strlcpy(s_nvs[i].key, key, sizeof(s_nvs[i].key));
            s_nvs[i].val[0] = '\0';
            s_nvs[i].present = true;
            s_nvs[i].writes = 0;
            return &s_nvs[i];
        }
    }
    return NULL;
}

static void nvs_seed(const char *key, const char *val)
{
    fake_nvs_entry_t *e = nvs_slot(key);
    strlcpy(e->val, val, sizeof(e->val));
}

static const char *nvs_get(const char *key)
{
    fake_nvs_entry_t *e = nvs_find(key);
    return e ? e->val : NULL;
}

static int nvs_writes(const char *key)
{
    fake_nvs_entry_t *e = nvs_find(key);
    return e ? e->writes : 0;
}

esp_err_t jettyd_nvs_write_str(const char *ns, const char *key, const char *value)
{
    (void)ns;
    fake_nvs_entry_t *e = nvs_slot(key);
    if (!e) return ESP_ERR_NO_MEM;
    strlcpy(e->val, value, sizeof(e->val));
    e->writes++;
    return ESP_OK;
}

esp_err_t jettyd_nvs_read_str(const char *ns, const char *key,
                               char *buf, size_t buf_len)
{
    (void)ns;
    const char *v = nvs_get(key);
    if (!v) return ESP_ERR_NOT_FOUND;
    strlcpy(buf, v, buf_len);
    return ESP_OK;
}

/* ── Pull in the code under test ──────────────────────────────────────── */

#define JETTYD_PORTAL_HOST_TEST 1
#include "../jettyd/src/wifi_portal.c"

/* ── Test framework ───────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("\n--- Test: %s ---\n", #name); \
    reset_all(); \
    test_##name(); \
    printf("--- %s: done ---\n", #name); \
} while (0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL at %s:%d — %s\n", __FILE__, __LINE__, #expr); \
        s_failed++; \
        return; \
    } \
} while (0)
#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_EQ(a, b)    ASSERT_TRUE((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE((a) != NULL && strcmp((a), (b)) == 0)
#define PASS() do { s_passed++; } while (0)

#define FAIL_THRESHOLD 5
#define BOOT_WINDOW_S  300u

static const char *DEVICE_KEY  = "dk_live_secret";
static const char *FLEET_TOKEN = "ft_live_token";
static const char *OLD_SSID    = "OldNet";
static const char *OLD_PASS    = "oldpass123";

static void reset_all(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    /* Pre-seed: a provisioned device with identity keys and WiFi creds */
    nvs_seed("wifi_ssid",   OLD_SSID);
    nvs_seed("wifi_pass",   OLD_PASS);
    nvs_seed("device_key",  DEVICE_KEY);
    nvs_seed("fleet_token", FLEET_TOKEN);
}

static void assert_identity_keys_untouched(void)
{
    ASSERT_TRUE(nvs_writes("device_key")  == 0);
    ASSERT_TRUE(nvs_writes("fleet_token") == 0);
    ASSERT_STR_EQ(nvs_get("device_key"),  DEVICE_KEY);
    ASSERT_STR_EQ(nvs_get("fleet_token"), FLEET_TOKEN);
}

/* ── Trigger state machine ──────────────────────────────────────────────── */

TEST(below_threshold_no_trigger) {
    /* fail_count < threshold → should_trigger must be false regardless of arming */
    ASSERT_FALSE(jettyd_portal_should_trigger(0, FAIL_THRESHOLD, true));
    ASSERT_FALSE(jettyd_portal_should_trigger(4, FAIL_THRESHOLD, true));
    PASS();
}

TEST(threshold_reached_not_armed_no_trigger) {
    /* Armed=false → portal must NOT trigger even when threshold is met */
    ASSERT_FALSE(jettyd_portal_should_trigger(FAIL_THRESHOLD, FAIL_THRESHOLD, false));
    ASSERT_FALSE(jettyd_portal_should_trigger(99,            FAIL_THRESHOLD, false));
    PASS();
}

TEST(threshold_reached_and_armed_triggers) {
    ASSERT_TRUE(jettyd_portal_should_trigger(FAIL_THRESHOLD, FAIL_THRESHOLD, true));
    ASSERT_TRUE(jettyd_portal_should_trigger(FAIL_THRESHOLD + 1, FAIL_THRESHOLD, true));
    PASS();
}

TEST(armed_fresh_device_no_ssid) {
    /* no_ssid=true → armed */
    ASSERT_TRUE(jettyd_portal_is_armed(true, false, false));
    PASS();
}

TEST(armed_within_boot_window) {
    /* in_boot_window=true → armed */
    ASSERT_TRUE(jettyd_portal_is_armed(false, true, false));
    PASS();
}

TEST(armed_button_held) {
    /* button_held_3s=true → armed */
    ASSERT_TRUE(jettyd_portal_is_armed(false, false, true));
    PASS();
}

TEST(not_armed_when_all_false) {
    /* No arming condition → not armed */
    ASSERT_FALSE(jettyd_portal_is_armed(false, false, false));
    PASS();
}

/* ── Credential validation ──────────────────────────────────────────────── */

TEST(valid_creds_accepted) {
    ASSERT_EQ(jettyd_portal_validate_creds("MySSID", "mypassword"), ESP_OK);
    PASS();
}

TEST(valid_ssid_at_32_accepted) {
    char ssid[33];
    memset(ssid, 'a', 32);
    ssid[32] = '\0';
    ASSERT_EQ(jettyd_portal_validate_creds(ssid, "pw"), ESP_OK);
    PASS();
}

TEST(empty_ssid_rejected) {
    esp_err_t err = jettyd_portal_validate_creds("", "pw");
    ASSERT_TRUE(err != ESP_OK);
    PASS();
}

TEST(ssid_over_32_rejected) {
    char ssid[34];
    memset(ssid, 'a', 33);
    ssid[33] = '\0';
    esp_err_t err = jettyd_portal_validate_creds(ssid, "pw");
    ASSERT_TRUE(err != ESP_OK);
    PASS();
}

TEST(password_over_64_rejected) {
    char pass[66];
    memset(pass, 'b', 65);
    pass[65] = '\0';
    esp_err_t err = jettyd_portal_validate_creds("SSID", pass);
    ASSERT_TRUE(err != ESP_OK);
    PASS();
}

TEST(null_ssid_rejected) {
    esp_err_t err = jettyd_portal_validate_creds(NULL, "pw");
    ASSERT_TRUE(err != ESP_OK);
    PASS();
}

TEST(null_password_accepted_as_open) {
    ASSERT_EQ(jettyd_portal_validate_creds("SSID", NULL), ESP_OK);
    PASS();
}

/* ── POST /save: NVS write correctness ───────────────────────────────────── */

TEST(save_valid_creds_writes_wifi_keys) {
    esp_err_t err = jettyd_portal_save_creds("NewNet", "newpass");
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(nvs_get("wifi_ssid"), "NewNet");
    ASSERT_STR_EQ(nvs_get("wifi_pass"), "newpass");
    ASSERT_TRUE(nvs_writes("wifi_ssid") == 1);
    ASSERT_TRUE(nvs_writes("wifi_pass") == 1);
    PASS();
}

TEST(save_empty_ssid_rejected_no_nvs_write) {
    esp_err_t err = jettyd_portal_save_creds("", "pw");
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes("wifi_ssid") == 0);
    ASSERT_TRUE(nvs_writes("wifi_pass") == 0);
    ASSERT_STR_EQ(nvs_get("wifi_ssid"), OLD_SSID); /* unchanged */
    PASS();
}

TEST(save_ssid_over_32_rejected) {
    char ssid[34];
    memset(ssid, 'a', 33);
    ssid[33] = '\0';
    esp_err_t err = jettyd_portal_save_creds(ssid, "pw");
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes("wifi_ssid") == 0);
    PASS();
}

TEST(save_password_over_64_rejected) {
    char pass[66];
    memset(pass, 'p', 65);
    pass[65] = '\0';
    esp_err_t err = jettyd_portal_save_creds("Net", pass);
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes("wifi_ssid") == 0);
    ASSERT_TRUE(nvs_writes("wifi_pass") == 0);
    PASS();
}

TEST(save_creds_never_touches_identity_keys) {
    jettyd_portal_save_creds("AnyNet", "anypass");
    assert_identity_keys_untouched();
    PASS();
}

TEST(save_open_network_stores_empty_password) {
    esp_err_t err = jettyd_portal_save_creds("OpenNet", "");
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(nvs_get("wifi_pass"), "");
    assert_identity_keys_untouched();
    PASS();
}

/* ── POST body parser ─────────────────────────────────────────────────────── */

TEST(parse_basic_ssid_and_password) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *body = "ssid=MyNet&password=Secret";
    esp_err_t err = jettyd_portal_parse_post_body(body, strlen(body), ssid, pass);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(ssid, "MyNet");
    ASSERT_STR_EQ(pass, "Secret");
    PASS();
}

TEST(parse_password_absent_defaults_empty) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *body = "ssid=JustSSID";
    esp_err_t err = jettyd_portal_parse_post_body(body, strlen(body), ssid, pass);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(ssid, "JustSSID");
    ASSERT_STR_EQ(pass, "");
    PASS();
}

TEST(parse_url_encoded_space_plus) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *body = "ssid=My+Net&password=my+pass";
    esp_err_t err = jettyd_portal_parse_post_body(body, strlen(body), ssid, pass);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(ssid, "My Net");
    ASSERT_STR_EQ(pass, "my pass");
    PASS();
}

TEST(parse_url_encoded_percent) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *body = "ssid=Net%21&password=p%40ss";
    esp_err_t err = jettyd_portal_parse_post_body(body, strlen(body), ssid, pass);
    ASSERT_EQ(err, ESP_OK);
    ASSERT_STR_EQ(ssid, "Net!");
    ASSERT_STR_EQ(pass, "p@ss");
    PASS();
}

TEST(parse_empty_ssid_field_rejected) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *body = "ssid=&password=pw";
    esp_err_t err = jettyd_portal_parse_post_body(body, strlen(body), ssid, pass);
    ASSERT_TRUE(err != ESP_OK);
    PASS();
}

/* ── SSID suffix builder ──────────────────────────────────────────────────── */

TEST(make_ap_ssid_uses_last_6_of_device_id) {
    char out[33] = {0};
    /* "device_abc123" = 13 chars; last 6 = "abc123" */
    jettyd_portal_make_ap_ssid("device_abc123", out, sizeof(out));
    ASSERT_STR_EQ(out, "jettyd-abc123");
    PASS();
}

TEST(make_ap_ssid_short_device_id) {
    char out[33] = {0};
    jettyd_portal_make_ap_ssid("abc", out, sizeof(out));
    /* Length < 6: use the whole id */
    ASSERT_STR_EQ(out, "jettyd-abc");
    PASS();
}

TEST(make_ap_ssid_empty_device_id_fallback) {
    char out[33] = {0};
    jettyd_portal_make_ap_ssid("", out, sizeof(out));
    ASSERT_STR_EQ(out, "jettyd-000000");
    PASS();
}

TEST(make_ap_ssid_null_device_id_fallback) {
    char out[33] = {0};
    jettyd_portal_make_ap_ssid(NULL, out, sizeof(out));
    ASSERT_STR_EQ(out, "jettyd-000000");
    PASS();
}

/* ── Idle timeout ──────────────────────────────────────────────────────────── */

TEST(idle_timeout_elapsed_returns_true) {
    /* last_activity = 0 (far in the past), timeout = 1 s
     * esp_timer_get_time() returns monotonic usec since boot — always > 1e6 */
    ASSERT_TRUE(jettyd_portal_is_timed_out(0, 1));
    PASS();
}

TEST(idle_timeout_recent_activity_returns_false) {
    int64_t now = esp_timer_get_time();
    ASSERT_FALSE(jettyd_portal_is_timed_out(now, 600));
    PASS();
}

TEST(idle_timeout_exactly_at_boundary) {
    /* last_activity = now - (timeout_s * 1e6): exactly at boundary → timed out */
    uint32_t timeout_s = 10;
    int64_t now = esp_timer_get_time();
    int64_t last = now - (int64_t)timeout_s * 1000000LL - 1;
    ASSERT_TRUE(jettyd_portal_is_timed_out(last, timeout_s));
    PASS();
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  SoftAP Portal Unit Tests (FLU-164)\n");
    printf("═══════════════════════════════════════\n");

    /* Trigger state machine */
    RUN_TEST(below_threshold_no_trigger);
    RUN_TEST(threshold_reached_not_armed_no_trigger);
    RUN_TEST(threshold_reached_and_armed_triggers);
    RUN_TEST(armed_fresh_device_no_ssid);
    RUN_TEST(armed_within_boot_window);
    RUN_TEST(armed_button_held);
    RUN_TEST(not_armed_when_all_false);

    /* Credential validation */
    RUN_TEST(valid_creds_accepted);
    RUN_TEST(valid_ssid_at_32_accepted);
    RUN_TEST(empty_ssid_rejected);
    RUN_TEST(ssid_over_32_rejected);
    RUN_TEST(password_over_64_rejected);
    RUN_TEST(null_ssid_rejected);
    RUN_TEST(null_password_accepted_as_open);

    /* POST /save NVS writes */
    RUN_TEST(save_valid_creds_writes_wifi_keys);
    RUN_TEST(save_empty_ssid_rejected_no_nvs_write);
    RUN_TEST(save_ssid_over_32_rejected);
    RUN_TEST(save_password_over_64_rejected);
    RUN_TEST(save_creds_never_touches_identity_keys);
    RUN_TEST(save_open_network_stores_empty_password);

    /* POST body parser */
    RUN_TEST(parse_basic_ssid_and_password);
    RUN_TEST(parse_password_absent_defaults_empty);
    RUN_TEST(parse_url_encoded_space_plus);
    RUN_TEST(parse_url_encoded_percent);
    RUN_TEST(parse_empty_ssid_field_rejected);

    /* SSID suffix builder */
    RUN_TEST(make_ap_ssid_uses_last_6_of_device_id);
    RUN_TEST(make_ap_ssid_short_device_id);
    RUN_TEST(make_ap_ssid_empty_device_id_fallback);
    RUN_TEST(make_ap_ssid_null_device_id_fallback);

    /* Idle timeout */
    RUN_TEST(idle_timeout_elapsed_returns_true);
    RUN_TEST(idle_timeout_recent_activity_returns_false);
    RUN_TEST(idle_timeout_exactly_at_boundary);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
