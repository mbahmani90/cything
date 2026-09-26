#include "paired_list.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_random.h"

#include "common/cy_log.h"

#define NVS_NS        "cy_sec"
#define NVS_KEY_LIST  "paired"
/* v2 adds owner_sub: ownership is bound to the account (sub), not the first
 * installId, so a reinstalled / second phone of the owner's account is owner
 * again after it re-pairs (doc/local-pairing-password.md "Device ownership").
 * v3 drops install_id / display_name for user_email (the owner's label in
 * LIST); a v2 blob is migrated on load, display_name becoming the label.
 * A v1 blob is a different layout/version and is discarded on load (re-pair). */
#define BLOB_VERSION  3

typedef struct {
    uint8_t  version;
    uint8_t  count;
    uint8_t  reserved[2];
    /* The owning account: any entry whose user_sub equals this is OWNER.
     * Set to the first phone to pair (bootstrap); empty when the list is. */
    char     owner_sub[PAIRED_SUB_MAX + 1];
    paired_entry_t entries[PAIRED_LIST_MAX];
} paired_blob_t;

/* The v2 layout, only to migrate a stored v2 blob. */
typedef struct {
    char     user_sub[PAIRED_SUB_MAX + 1];
    char     install_id[32 + 1];
    char     display_name[32 + 1];
    uint8_t  key[PAIRED_KEY_LEN];
    uint32_t paired_at;
    uint8_t  role;
} paired_entry_v2_t;

typedef struct {
    uint8_t  version;
    uint8_t  count;
    uint8_t  reserved[2];
    char     owner_sub[PAIRED_SUB_MAX + 1];
    paired_entry_v2_t entries[PAIRED_LIST_MAX];
} paired_blob_v2_t;

static paired_blob_t     s_list;
static SemaphoreHandle_t s_mutex = NULL;

static void copy_field(char *dst, size_t dst_size, const char *src);

/* Serialize the whole list. `count` entries are written; the tail is left
 * out so the blob shrinks with the list. Mutex held. */
static esp_err_t nvs_store(void){
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    size_t n = offsetof(paired_blob_t, entries) + (size_t)s_list.count * sizeof(paired_entry_t);
    if(s_list.count == 0){
        err = nvs_erase_key(h, NVS_KEY_LIST);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }else{
        err = nvs_set_blob(h, NVS_KEY_LIST, &s_list, n);
    }
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if(err != ESP_OK){
        CY_LOGE(TCP_SERVER_DB, "paired_list: store failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Read a v2 blob and convert it into s_list. Returns true if s_list now
 * holds the migrated list (the caller persists it as v3). */
static bool migrate_v2(nvs_handle_t h){
    paired_blob_v2_t *old = calloc(1, sizeof(*old));
    if(old == NULL) return false;
    size_t n = sizeof(*old);
    bool ok = nvs_get_blob(h, NVS_KEY_LIST, old, &n) == ESP_OK &&
              old->version == 2 && old->count <= PAIRED_LIST_MAX &&
              n == offsetof(paired_blob_v2_t, entries) + (size_t)old->count * sizeof(paired_entry_v2_t);
    if(ok){
        memset(&s_list, 0, sizeof(s_list));
        s_list.version = BLOB_VERSION;
        s_list.count   = old->count;
        memcpy(s_list.owner_sub, old->owner_sub, sizeof(s_list.owner_sub));
        for(int i = 0 ; i < old->count ; i++){
            paired_entry_t *e = &s_list.entries[i];
            memcpy(e->user_sub, old->entries[i].user_sub, sizeof(e->user_sub));
            copy_field(e->user_email, sizeof(e->user_email), old->entries[i].display_name);
            memcpy(e->key, old->entries[i].key, PAIRED_KEY_LEN);
            e->paired_at = old->entries[i].paired_at;
            e->role      = old->entries[i].role;
        }
    }
    memset(old, 0, sizeof(*old));   /* held keys */
    free(old);
    return ok;
}

/* Returns true if the loaded list must be written back (migrated). */
static bool nvs_load(void){
    memset(&s_list, 0, sizeof(s_list));
    s_list.version = BLOB_VERSION;

    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof(s_list);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LIST, &s_list, &n);

    /* A v2 blob can be larger than a v3 one (INVALID_LENGTH) or fit. */
    bool migrated = false;
    if(err == ESP_ERR_NVS_INVALID_LENGTH || (err == ESP_OK && s_list.version == 2)){
        migrated = migrate_v2(h);
        if(migrated){
            CY_LOGW(TCP_SERVER_DB, "paired_list: migrated v2 blob (%u entries) to v%u",
                    s_list.count, BLOB_VERSION);
        }
    }
    nvs_close(h);

    if(!migrated &&
       (err != ESP_OK || s_list.version != BLOB_VERSION || s_list.count > PAIRED_LIST_MAX ||
        n != offsetof(paired_blob_t, entries) + (size_t)s_list.count * sizeof(paired_entry_t))){
        if(err != ESP_ERR_NVS_NOT_FOUND){
            CY_LOGW(TCP_SERVER_DB, "paired_list: stored blob unusable (%s, v%u, n=%u), starting empty",
                    esp_err_to_name(err), s_list.version, s_list.count);
        }
        memset(&s_list, 0, sizeof(s_list));
        s_list.version = BLOB_VERSION;
    }
    return migrated;
}

static void copy_field(char *dst, size_t dst_size, const char *src){
    if(src == NULL){ dst[0] = 0; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

/* Role an account gets: OWNER iff it is the owning account. Mutex held. */
static uint8_t role_for_locked(const char *user_sub){
    if(s_list.owner_sub[0] != 0 &&
       strncmp(user_sub, s_list.owner_sub, PAIRED_SUB_MAX) == 0){
        return PAIRED_ROLE_OWNER;
    }
    return PAIRED_ROLE_USER;
}

static int find_locked(const char *user_sub){
    for(int i = 0 ; i < s_list.count ; i++){
        if(strncmp(s_list.entries[i].user_sub, user_sub, PAIRED_SUB_MAX) == 0) return i;
    }
    return -1;
}

/* Firmware before per-account keys kept one entry per (userSub, installId).
 * Collapse each account to its oldest entry (same layout, so the v2 blob is
 * kept): the dropped phones get ERR:UNKNOWN on AUTH and re-pair, which hands
 * them the surviving key. Returns true if anything was removed. Mutex held. */
static bool dedupe_accounts_locked(void){
    bool changed = false;
    for(int i = 0 ; i < s_list.count ; i++){
        int j = i + 1;
        while(j < s_list.count){
            if(strncmp(s_list.entries[j].user_sub, s_list.entries[i].user_sub, PAIRED_SUB_MAX) == 0){
                for(int k = j ; k < s_list.count - 1 ; k++) s_list.entries[k] = s_list.entries[k + 1];
                memset(&s_list.entries[s_list.count - 1], 0, sizeof(paired_entry_t));
                s_list.count--;
                changed = true;
            }else{
                j++;
            }
        }
    }
    return changed;
}

/* ---- public ----------------------------------------------------------- */

void paired_list_init(void){
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    bool dirty = nvs_load();
    if(dedupe_accounts_locked()){
        CY_LOGW(TCP_SERVER_DB, "paired_list: collapsed per-phone entries to one per account");
        dirty = true;
    }
    if(dirty) nvs_store();
    CY_LOGI(TCP_SERVER_DB, "paired_list: %u entr%s", s_list.count, s_list.count == 1 ? "y" : "ies");
}

int paired_list_count(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_list.count;
    xSemaphoreGive(s_mutex);
    return n;
}

int paired_list_find(const char *user_sub){
    if(user_sub == NULL) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int i = find_locked(user_sub);
    xSemaphoreGive(s_mutex);
    return i;
}

bool paired_list_get(int index, paired_entry_t *out){
    bool ok = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if(index >= 0 && index < s_list.count){
        *out = s_list.entries[index];
        ok = true;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

int paired_list_add(const char *user_sub, const char *user_email,
                    uint8_t key[PAIRED_KEY_LEN],
                    uint32_t paired_at, paired_role_t *role_out){
    if(user_sub == NULL || key == NULL || user_sub[0] == 0) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int i = find_locked(user_sub);

    /* First phone to pair claims ownership for its account (bootstrap). */
    bool claimed_owner = false;
    if(s_list.owner_sub[0] == 0){
        copy_field(s_list.owner_sub, sizeof(s_list.owner_sub), user_sub);
        claimed_owner = true;
    }

    paired_entry_t *e;
    paired_entry_t before = {0};
    bool appended = false;
    if(i >= 0){
        /* The account is already paired (another phone, or this one again):
         * it keeps its key, the caller gets it back. */
        e = &s_list.entries[i];
        before = *e;
        memcpy(key, e->key, PAIRED_KEY_LEN);
    }else{
        if(s_list.count >= PAIRED_LIST_MAX){
            if(claimed_owner) s_list.owner_sub[0] = 0;
            xSemaphoreGive(s_mutex);
            CY_LOGW(TCP_SERVER_DB, "paired_list: full (%d)", PAIRED_LIST_MAX);
            return -1;
        }
        i = s_list.count;
        e = &s_list.entries[i];
        memset(e, 0, sizeof(*e));
        copy_field(e->user_sub,   sizeof(e->user_sub),   user_sub);
        memcpy(e->key, key, PAIRED_KEY_LEN);
        s_list.count++;
        appended = true;
    }
    if(user_email != NULL && user_email[0] != 0){
        copy_field(e->user_email, sizeof(e->user_email), user_email);
    }
    e->paired_at = paired_at;
    /* Ownership is by account: role follows owner_sub, not insertion order. */
    e->role = role_for_locked(user_sub);

    esp_err_t err = nvs_store();
    if(err != ESP_OK){
        /* Undo so RAM and flash stay in step. */
        if(appended){
            memset(e, 0, sizeof(*e));
            s_list.count--;
        }else{
            *e = before;
        }
        memset(&before, 0, sizeof(before));
        if(claimed_owner) s_list.owner_sub[0] = 0;
        xSemaphoreGive(s_mutex);
        return -1;
    }
    memset(&before, 0, sizeof(before));
    if(role_out) *role_out = (paired_role_t)e->role;
    xSemaphoreGive(s_mutex);

    CY_LOGI(TCP_SERVER_DB, "paired_list: #%d %s (%s) role %u", i, e->user_sub, e->user_email, e->role);
    return i;
}

esp_err_t paired_list_remove(int index){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if(index < 0 || index >= s_list.count){
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    /* Keep the array dense: shift the tail down, wipe the freed slot so no
     * key lingers past the count. */
    for(int i = index ; i < s_list.count - 1 ; i++){
        s_list.entries[i] = s_list.entries[i + 1];
    }
    memset(&s_list.entries[s_list.count - 1], 0, sizeof(paired_entry_t));
    s_list.count--;
    esp_err_t err = nvs_store();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t paired_list_rotate_owner_key(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t old[PAIRED_KEY_LEN];
    int rotated = -1;
    for(int i = 0 ; i < s_list.count ; i++){
        if(s_list.entries[i].role == PAIRED_ROLE_OWNER){
            memcpy(old, s_list.entries[i].key, PAIRED_KEY_LEN);
            esp_fill_random(s_list.entries[i].key, PAIRED_KEY_LEN);
            rotated = i;
            break;                       /* one entry per account */
        }
    }
    esp_err_t err = ESP_OK;
    if(rotated >= 0){
        err = nvs_store();
        if(err != ESP_OK) memcpy(s_list.entries[rotated].key, old, PAIRED_KEY_LEN);
    }
    memset(old, 0, sizeof(old));
    xSemaphoreGive(s_mutex);
    if(rotated >= 0 && err == ESP_OK){
        CY_LOGI(TCP_SERVER_DB, "paired_list: owner key rotated");
    }
    return err;
}

esp_err_t paired_list_clear(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_list, 0, sizeof(s_list));
    s_list.version = BLOB_VERSION;
    esp_err_t err = nvs_store();
    xSemaphoreGive(s_mutex);
    return err;
}
