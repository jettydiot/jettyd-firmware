/**
 * @file test_wifi_reconfigure.c
 * @brief Host unit tests for jettyd_wifi_reconfigure() — the wifi.set runtime
 *        WiFi credential update with rollback (FLU-163).
 *
 * White-box pattern (mirrors test_servo.c / test_camera.c): wifi.c is compiled
 * directly with JETTYD_WIFI_HOST_TEST so only the target-independent
 * reconfigure logic is pulled in. The WiFi radio, NVS, and provisioning layers
 * are supplied below as controllable stubs:
 *
 *   - a fake NVS map for the jettyd_prov namespace that records per-key writes,
 *   - a fake provisioning state (the credentials currently in RAM),
 *   - fake WiFi seams that log connect attempts and let a test decide which
 *     SSID "connects" and which times out.
 *
 * This lets us assert the full contract without hardware: NVS is updated on the
 * happy path, invalid payloads write nothing, a connect timeout restores the
 * exact prior NVS values and reconnects to the previous network, and the
 * provisioning identity keys (device_key / fleet_token) are never written.
 */

#include "mocks/esp_idf_stubs.h"

#include "jettyd_provision.h"
#include "jettyd_nvs.h"
#include "jettyd_wifi.h"

/* ── Fake NVS (jettyd_prov namespace) ─────────────────────────────────────── */

#define FAKE_NVS_MAX 16

typedef struct {
    char key[32];
    char val[129];
    bool present;
    int  writes;   /* number of jettyd_nvs_write_str() calls for this key */
} fake_nvs_entry_t;

static fake_nvs_entry_t s_nvs[FAKE_NVS_MAX];

static fake_nvs_entry_t *nvs_find(const char *key)
{
    for (int i = 0; i < FAKE_NVS_MAX; i++) {
        if (s_nvs[i].present && strcmp(s_nvs[i].key, key) == 0) {
            return &s_nvs[i];
        }
    }
    return NULL;
}

static fake_nvs_entry_t *nvs_slot(const char *key)
{
    fake_nvs_entry_t *e = nvs_find(key);
    if (e) {
        return e;
    }
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

/* Seed a key as if flashed at provisioning time — does not count as a write. */
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

/* Seam under test: the SUT persists credentials through this. */
esp_err_t jettyd_nvs_write_str(const char *ns, const char *key, const char *value)
{
    (void)ns;
    fake_nvs_entry_t *e = nvs_slot(key);
    if (!e) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(e->val, value, sizeof(e->val));
    e->writes++;
    return ESP_OK;
}

esp_err_t jettyd_nvs_read_str(const char *ns, const char *key, char *buf, size_t buf_len)
{
    (void)ns;
    const char *v = nvs_get(key);
    if (!v) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(buf, v, buf_len);
    return ESP_OK;
}

/* ── Fake provisioning state (credentials currently in RAM) ───────────────── */

static jettyd_provision_state_t s_prov;

const jettyd_provision_state_t *jettyd_provision_get_state(void)
{
    return &s_prov;
}

/* Not used by reconfigure (which persists via jettyd_nvs_write_str to avoid
 * touching identity keys) but defined so the link never depends on it. If it is
 * ever called it would be a regression the key-safety tests would catch. */
esp_err_t jettyd_provision_store(const jettyd_provision_state_t *state)
{
    (void)state;
    return ESP_OK;
}

/* ── Fake WiFi radio seams ────────────────────────────────────────────────── */

#define CONN_LOG_MAX 8

static char s_conn_ssid[CONN_LOG_MAX][33];
static char s_conn_pass[CONN_LOG_MAX][65];
static int  s_conn_count;
static char s_last_target[33];
static char s_bad_ssid[33];   /* wait_connected() times out for this SSID */
static jettyd_wifi_state_t s_wifi_state;
static int  s_disconnect_calls;

esp_err_t jettyd_wifi_connect_with(const char *ssid, const char *password)
{
    if (s_conn_count < CONN_LOG_MAX) {
        strlcpy(s_conn_ssid[s_conn_count], ssid ? ssid : "", sizeof(s_conn_ssid[0]));
        strlcpy(s_conn_pass[s_conn_count], password ? password : "", sizeof(s_conn_pass[0]));
        s_conn_count++;
    }
    strlcpy(s_last_target, ssid ? ssid : "", sizeof(s_last_target));
    s_wifi_state = JETTYD_WIFI_CONNECTING;
    return ESP_OK;
}

esp_err_t jettyd_wifi_wait_connected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (s_bad_ssid[0] != '\0' && strcmp(s_last_target, s_bad_ssid) == 0) {
        s_wifi_state = JETTYD_WIFI_FAILED;
        return ESP_ERR_TIMEOUT;
    }
    s_wifi_state = JETTYD_WIFI_CONNECTED;
    return ESP_OK;
}

esp_err_t jettyd_wifi_disconnect(void)
{
    s_disconnect_calls++;
    s_wifi_state = JETTYD_WIFI_DISCONNECTED;
    return ESP_OK;
}

jettyd_wifi_state_t jettyd_wifi_get_state(void)
{
    return s_wifi_state;
}

/* ── Pull in the code under test ──────────────────────────────────────────── */

#define JETTYD_WIFI_HOST_TEST 1
#include "../jettyd/src/wifi.c"

/* ── Test framework ───────────────────────────────────────────────────────── */

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
#define ASSERT_STR_EQ(a, b) ASSERT_TRUE((a) != NULL && strcmp((a), (b)) == 0)
#define PASS() do { s_passed++; } while (0)

#define TIMEOUT_MS 30000u

static const char *OLD_SSID = "OldNet";
static const char *OLD_PASS = "oldpass123";
static const char *DEVICE_KEY = "dk_live_secret";
static const char *FLEET_TOKEN = "ft_live_secret";

static void reset_all(void)
{
    memset(s_nvs, 0, sizeof(s_nvs));
    memset(&s_prov, 0, sizeof(s_prov));
    memset(s_conn_ssid, 0, sizeof(s_conn_ssid));
    memset(s_conn_pass, 0, sizeof(s_conn_pass));
    s_conn_count = 0;
    s_last_target[0] = '\0';
    s_bad_ssid[0] = '\0';
    s_disconnect_calls = 0;
    s_wifi_state = JETTYD_WIFI_CONNECTED;

    /* A provisioned device currently on OLD_SSID. */
    strlcpy(s_prov.wifi_ssid, OLD_SSID, sizeof(s_prov.wifi_ssid));
    strlcpy(s_prov.wifi_pass, OLD_PASS, sizeof(s_prov.wifi_pass));
    strlcpy(s_prov.device_key, DEVICE_KEY, sizeof(s_prov.device_key));
    strlcpy(s_prov.fleet_token, FLEET_TOKEN, sizeof(s_prov.fleet_token));
    strlcpy(s_prov.tenant_id, "tenantA", sizeof(s_prov.tenant_id));
    s_prov.provisioned = true;

    /* NVS as flashed: matches the current provisioning state. */
    nvs_seed(JETTYD_PROV_KEY_WIFI_SSID, OLD_SSID);
    nvs_seed(JETTYD_PROV_KEY_WIFI_PASS, OLD_PASS);
    nvs_seed(JETTYD_PROV_KEY_DEVICE_KEY, DEVICE_KEY);
    nvs_seed(JETTYD_PROV_KEY_FLEET_TOKEN, FLEET_TOKEN);
}

static void assert_identity_keys_untouched(void)
{
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_DEVICE_KEY) == 0);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_FLEET_TOKEN) == 0);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_DEVICE_KEY), DEVICE_KEY);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_FLEET_TOKEN), FLEET_TOKEN);
}

/* ── Happy path ───────────────────────────────────────────────────────────── */

TEST(valid_payload_updates_nvs_and_connects) {
    esp_err_t err = jettyd_wifi_reconfigure("NewNet", "newpass456", TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_OK);

    /* NVS updated to the new credentials. */
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), "NewNet");
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_PASS), "newpass456");

    /* connect_with() was called with the new credentials. */
    ASSERT_TRUE(s_conn_count >= 1);
    ASSERT_STR_EQ(s_conn_ssid[0], "NewNet");
    ASSERT_STR_EQ(s_conn_pass[0], "newpass456");

    /* No rollback: only the new network was ever targeted. */
    ASSERT_TRUE(s_conn_count == 1);
    assert_identity_keys_untouched();
    PASS();
}

TEST(ssid_at_32_bytes_is_accepted) {
    char ssid[33];
    memset(ssid, 'a', 32);
    ssid[32] = '\0';
    esp_err_t err = jettyd_wifi_reconfigure(ssid, "pw", TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_OK);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), ssid);
    PASS();
}

TEST(open_network_clears_password) {
    esp_err_t err = jettyd_wifi_reconfigure("OpenNet", "", TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_OK);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), "OpenNet");
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_PASS), "");
    ASSERT_STR_EQ(s_conn_ssid[0], "OpenNet");
    ASSERT_STR_EQ(s_conn_pass[0], "");
    PASS();
}

TEST(null_password_treated_as_open_network) {
    esp_err_t err = jettyd_wifi_reconfigure("OpenNet", NULL, TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_OK);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_PASS), "");
    PASS();
}

/* ── Payload validation (must not write NVS) ──────────────────────────────── */

TEST(empty_ssid_rejected_no_nvs_write) {
    esp_err_t err = jettyd_wifi_reconfigure("", "pw", TIMEOUT_MS);
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_WIFI_SSID) == 0);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_WIFI_PASS) == 0);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), OLD_SSID);
    ASSERT_TRUE(s_conn_count == 0);
    assert_identity_keys_untouched();
    PASS();
}

TEST(ssid_over_32_bytes_rejected_no_nvs_write) {
    char ssid[64];
    memset(ssid, 'a', 33);
    ssid[33] = '\0';
    esp_err_t err = jettyd_wifi_reconfigure(ssid, "pw", TIMEOUT_MS);
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_WIFI_SSID) == 0);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), OLD_SSID);
    ASSERT_TRUE(s_conn_count == 0);
    assert_identity_keys_untouched();
    PASS();
}

TEST(password_over_64_bytes_rejected_no_nvs_write) {
    char pass[80];
    memset(pass, 'b', 65);
    pass[65] = '\0';
    esp_err_t err = jettyd_wifi_reconfigure("NewNet", pass, TIMEOUT_MS);
    ASSERT_TRUE(err != ESP_OK);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_WIFI_SSID) == 0);
    ASSERT_TRUE(nvs_writes(JETTYD_PROV_KEY_WIFI_PASS) == 0);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), OLD_SSID);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_PASS), OLD_PASS);
    ASSERT_TRUE(s_conn_count == 0);
    assert_identity_keys_untouched();
    PASS();
}

/* ── Rollback ─────────────────────────────────────────────────────────────── */

TEST(connect_timeout_rolls_back_to_exact_prior_nvs) {
    s_bad_ssid[0] = '\0';
    strlcpy(s_bad_ssid, "NewNet", sizeof(s_bad_ssid)); /* new network never connects */

    esp_err_t err = jettyd_wifi_reconfigure("NewNet", "newpass456", TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_FAIL);

    /* NVS restored to the exact previous values. */
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_SSID), OLD_SSID);
    ASSERT_STR_EQ(nvs_get(JETTYD_PROV_KEY_WIFI_PASS), OLD_PASS);

    /* Attempted the new network, then reconnected to the previous one. */
    ASSERT_TRUE(s_conn_count == 2);
    ASSERT_STR_EQ(s_conn_ssid[0], "NewNet");
    ASSERT_STR_EQ(s_conn_ssid[1], OLD_SSID);
    ASSERT_STR_EQ(s_conn_pass[1], OLD_PASS);

    assert_identity_keys_untouched();
    PASS();
}

TEST(rollback_preserves_identity_keys) {
    strlcpy(s_bad_ssid, "NewNet", sizeof(s_bad_ssid));
    esp_err_t err = jettyd_wifi_reconfigure("NewNet", "pw", TIMEOUT_MS);
    ASSERT_TRUE(err == ESP_FAIL);
    assert_identity_keys_untouched();
    PASS();
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  wifi.set / reconfigure Unit Tests\n");
    printf("═══════════════════════════════════════\n");

    RUN_TEST(valid_payload_updates_nvs_and_connects);
    RUN_TEST(ssid_at_32_bytes_is_accepted);
    RUN_TEST(open_network_clears_password);
    RUN_TEST(null_password_treated_as_open_network);
    RUN_TEST(empty_ssid_rejected_no_nvs_write);
    RUN_TEST(ssid_over_32_bytes_rejected_no_nvs_write);
    RUN_TEST(password_over_64_bytes_rejected_no_nvs_write);
    RUN_TEST(connect_timeout_rolls_back_to_exact_prior_nvs);
    RUN_TEST(rollback_preserves_identity_keys);

    printf("\n═══════════════════════════════════════\n");
    printf("  Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("═══════════════════════════════════════\n");

    return s_failed > 0 ? 1 : 0;
}
