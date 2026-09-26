#include "device_password.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_srp.h"

#include "common/cy_log.h"
#include "device_config/cy_config.h"

#define NVS_NS         "cy_sec"
#define NVS_KEY_SALT   "pw_salt"
#define NVS_KEY_VER    "pw_ver"
#define NVS_KEY_FAILS  "pw_fails"   /* u8:  consecutive global failures       */
#define NVS_KEY_LOCK   "pw_lock"    /* u32: lock seconds left at last write   */

/* RAM copy of the stored pair. */
static uint8_t  s_salt[DEVICE_PW_SALT_LEN];
static uint8_t  s_verifier[DEVICE_PW_VERIFIER_MAX];
static size_t   s_salt_len = 0;
static size_t   s_verifier_len = 0;

/* Pair for cy_initial_password, derived on first open-device PAKE; the salt
 * is fresh every boot. */
static uint8_t  s_init_salt[DEVICE_PW_SALT_LEN];
static uint8_t  s_init_verifier[DEVICE_PW_VERIFIER_MAX];
static size_t   s_init_verifier_len = 0;

/* Lockout state. Times are esp_timer microseconds (monotonic since boot). */
typedef struct {
    uint32_t ip;
    uint8_t  fails;
    int64_t  until_us;
} ip_slot_t;

#define IP_SLOTS 8
static ip_slot_t s_ip[IP_SLOTS];
static uint8_t   s_global_fails = 0;
static int64_t   s_global_until_us = 0;

static SemaphoreHandle_t s_mutex = NULL;

static int64_t now_us(void){ return esp_timer_get_time(); }

/* Lock length for the n-th failure past the threshold: base, 2x, 4x … cap. */
static uint32_t lock_seconds(uint8_t fails, uint8_t threshold){
    if(fails < threshold) return 0;
    uint32_t s = DEVICE_PW_LOCK_BASE_S;
    for(uint8_t i = threshold ; i < fails && s < DEVICE_PW_LOCK_MAX_S ; i++){
        s *= 2;
    }
    return s > DEVICE_PW_LOCK_MAX_S ? DEVICE_PW_LOCK_MAX_S : s;
}

static uint32_t remaining_s(int64_t until_us){
    int64_t d = until_us - now_us();
    if(d <= 0) return 0;
    return (uint32_t)((d + 999999) / 1000000);
}

/* ---- NVS -------------------------------------------------------------- */

static esp_err_t nvs_load(void){
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if(err != ESP_OK) return err;   /* namespace absent = no password yet */

    size_t n = sizeof(s_salt);
    if(nvs_get_blob(h, NVS_KEY_SALT, s_salt, &n) == ESP_OK){
        s_salt_len = n;
        n = sizeof(s_verifier);
        if(nvs_get_blob(h, NVS_KEY_VER, s_verifier, &n) == ESP_OK){
            s_verifier_len = n;
        }else{
            s_salt_len = 0;
        }
    }

    uint8_t fails = 0;
    uint32_t lock_s = 0;
    nvs_get_u8(h, NVS_KEY_FAILS, &fails);
    nvs_get_u32(h, NVS_KEY_LOCK, &lock_s);
    s_global_fails = fails;
    if(lock_s > 0){
        s_global_until_us = now_us() + (int64_t)lock_s * 1000000;
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_store_pair(const uint8_t *salt, size_t salt_len,
                                const uint8_t *ver, size_t ver_len){
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    if(salt == NULL){
        nvs_erase_key(h, NVS_KEY_SALT);
        nvs_erase_key(h, NVS_KEY_VER);
    }else{
        err = nvs_set_blob(h, NVS_KEY_SALT, salt, salt_len);
        if(err == ESP_OK) err = nvs_set_blob(h, NVS_KEY_VER, ver, ver_len);
    }
    if(err == ESP_OK) err = nvs_set_u8 (h, NVS_KEY_FAILS, 0);
    if(err == ESP_OK) err = nvs_set_u32(h, NVS_KEY_LOCK, 0);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Persist the global counter + how long the lock still has to run, so a
 * reboot mid-lockout resumes it rather than clearing it. */
static void nvs_store_lockout(void){
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8 (h, NVS_KEY_FAILS, s_global_fails);
    nvs_set_u32(h, NVS_KEY_LOCK, remaining_s(s_global_until_us));
    nvs_commit(h);
    nvs_close(h);
}

/* ---- public ----------------------------------------------------------- */

void device_password_init(void){
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    memset(s_ip, 0, sizeof(s_ip));
    nvs_load();
    CY_LOGI(TCP_SERVER_DB, "device_password: %s, global fails %u, lock %us",
            s_verifier_len ? "set" : "not set", s_global_fails, (unsigned)remaining_s(s_global_until_us));
}

bool device_password_is_set(void){
    return s_salt_len > 0 && s_verifier_len > 0;
}

esp_err_t device_password_set(const char *password, size_t len){
    if(password == NULL || len < DEVICE_PW_MIN_LEN || len > DEVICE_PW_MAX_LEN){
        return ESP_ERR_INVALID_SIZE;
    }

    char *salt = NULL;
    char *ver  = NULL;
    int   ver_len = 0;
    esp_err_t err = esp_srp_gen_salt_verifier(DEVICE_PW_SRP_IDENTITY, strlen(DEVICE_PW_SRP_IDENTITY),
                                              password, (int)len,
                                              &salt, DEVICE_PW_SALT_LEN,
                                              &ver, &ver_len);
    if(err != ESP_OK){
        CY_LOGE(TCP_SERVER_DB, "device_password: verifier generation failed: %s", esp_err_to_name(err));
        return err;
    }
    if(ver_len <= 0 || ver_len > DEVICE_PW_VERIFIER_MAX){
        free(salt); free(ver);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    err = nvs_store_pair((const uint8_t *)salt, DEVICE_PW_SALT_LEN, (const uint8_t *)ver, (size_t)ver_len);
    if(err == ESP_OK){
        memcpy(s_salt, salt, DEVICE_PW_SALT_LEN);
        memcpy(s_verifier, ver, (size_t)ver_len);
        s_salt_len = DEVICE_PW_SALT_LEN;
        s_verifier_len = (size_t)ver_len;
        s_global_fails = 0;
        s_global_until_us = 0;
        memset(s_ip, 0, sizeof(s_ip));
    }
    xSemaphoreGive(s_mutex);

    free(salt);
    free(ver);
    CY_LOGI(TCP_SERVER_DB, "device_password: %s", err == ESP_OK ? "set" : esp_err_to_name(err));
    return err;
}

esp_err_t device_password_clear(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = nvs_store_pair(NULL, 0, NULL, 0);
    memset(s_salt, 0, sizeof(s_salt));
    memset(s_verifier, 0, sizeof(s_verifier));
    s_salt_len = s_verifier_len = 0;
    s_global_fails = 0;
    s_global_until_us = 0;
    memset(s_ip, 0, sizeof(s_ip));
    xSemaphoreGive(s_mutex);
    return err;
}

/* Mutex held. */
static bool derive_initial_locked(void){
    if(s_init_verifier_len > 0) return true;

    char *salt = NULL;
    char *ver  = NULL;
    int   ver_len = 0;
    esp_err_t err = esp_srp_gen_salt_verifier(DEVICE_PW_SRP_IDENTITY, strlen(DEVICE_PW_SRP_IDENTITY),
                                              cy_initial_password, (int)strlen(cy_initial_password),
                                              &salt, DEVICE_PW_SALT_LEN,
                                              &ver, &ver_len);
    bool ok = (err == ESP_OK && ver_len > 0 && ver_len <= DEVICE_PW_VERIFIER_MAX);
    if(ok){
        memcpy(s_init_salt, salt, DEVICE_PW_SALT_LEN);
        memcpy(s_init_verifier, ver, (size_t)ver_len);
        s_init_verifier_len = (size_t)ver_len;
    }else{
        CY_LOGE(TCP_SERVER_DB, "device_password: initial-password verifier failed: %s", esp_err_to_name(err));
    }
    free(salt);
    free(ver);
    return ok;
}

bool device_password_get(const uint8_t **salt, size_t *salt_len,
                         const uint8_t **verifier, size_t *verifier_len){
    if(device_password_is_set()){
        *salt = s_salt;             *salt_len = s_salt_len;
        *verifier = s_verifier;     *verifier_len = s_verifier_len;
        return true;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = derive_initial_locked();
    xSemaphoreGive(s_mutex);
    if(!ok) return false;
    *salt = s_init_salt;            *salt_len = DEVICE_PW_SALT_LEN;
    *verifier = s_init_verifier;    *verifier_len = s_init_verifier_len;
    return true;
}

/* ---- rate limiting ---------------------------------------------------- */

/* Slot for `ip`, or the least-recently-locked free-ish slot. Mutex held. */
static ip_slot_t *ip_slot(uint32_t ip){
    ip_slot_t *victim = &s_ip[0];
    for(int i = 0 ; i < IP_SLOTS ; i++){
        if(s_ip[i].ip == ip) return &s_ip[i];
        if(s_ip[i].ip == 0) { victim = &s_ip[i]; break; }
        if(s_ip[i].until_us < victim->until_us) victim = &s_ip[i];
    }
    memset(victim, 0, sizeof(*victim));
    victim->ip = ip;
    return victim;
}

uint32_t device_password_lock_remaining_s(uint32_t peer_ip){
    if(!device_password_is_set()) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t g = remaining_s(s_global_until_us);
    uint32_t p = 0;
    for(int i = 0 ; i < IP_SLOTS ; i++){
        if(s_ip[i].ip == peer_ip){ p = remaining_s(s_ip[i].until_us); break; }
    }
    xSemaphoreGive(s_mutex);
    return g > p ? g : p;
}

void device_password_note_failure(uint32_t peer_ip){
    /* A wrong guess at the public initial password is not an attack. */
    if(!device_password_is_set()) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    ip_slot_t *slot = ip_slot(peer_ip);
    if(slot->fails < 255) slot->fails++;
    uint32_t ls = lock_seconds(slot->fails, DEVICE_PW_IP_MAX_FAILS);
    if(ls) slot->until_us = now_us() + (int64_t)ls * 1000000;

    if(s_global_fails < 255) s_global_fails++;
    uint32_t gs = lock_seconds(s_global_fails, DEVICE_PW_GLOBAL_MAX_FAILS);
    if(gs) s_global_until_us = now_us() + (int64_t)gs * 1000000;
    nvs_store_lockout();

    CY_LOGW(TCP_SERVER_DB, "device_password: bad proof from %08x (ip fails %u lock %us; global fails %u lock %us)",
            (unsigned)peer_ip, slot->fails, (unsigned)ls, s_global_fails, (unsigned)gs);
    xSemaphoreGive(s_mutex);
}

void device_password_note_success(uint32_t peer_ip){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for(int i = 0 ; i < IP_SLOTS ; i++){
        if(s_ip[i].ip == peer_ip){ memset(&s_ip[i], 0, sizeof(s_ip[i])); break; }
    }
    bool dirty = (s_global_fails != 0 || s_global_until_us != 0);
    s_global_fails = 0;
    s_global_until_us = 0;
    if(dirty) nvs_store_lockout();
    xSemaphoreGive(s_mutex);
}
