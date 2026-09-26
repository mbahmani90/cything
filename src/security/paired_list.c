#include "paired_list.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "common/cy_log.h"

#define NVS_NS        "cy_sec"
#define NVS_KEY_LIST  "paired"
/* v2 adds owner_sub: ownership is bound to the account (sub), not the first
 * installId, so a reinstalled / second phone of the owner's account is owner
 * again after it re-pairs (doc/local-pairing-password.md "Device ownership").
 * A v1 blob is a different layout/version and is discarded on load (re-pair). */
#define BLOB_VERSION  2

typedef struct {
    uint8_t  version;
    uint8_t  count;
    uint8_t  reserved[2];
    /* The owning account: any entry whose user_sub equals this is OWNER.
     * Set to the first phone to pair (bootstrap); empty when the list is. */
    char     owner_sub[PAIRED_SUB_MAX + 1];
    paired_entry_t entries[PAIRED_LIST_MAX];
} paired_blob_t;

static paired_blob_t     s_list;
static SemaphoreHandle_t s_mutex = NULL;

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

static void nvs_load(void){
    memset(&s_list, 0, sizeof(s_list));
    s_list.version = BLOB_VERSION;

    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_list);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LIST, &s_list, &n);
    nvs_close(h);

    if(err != ESP_OK || s_list.version != BLOB_VERSION || s_list.count > PAIRED_LIST_MAX ||
       n != offsetof(paired_blob_t, entries) + (size_t)s_list.count * sizeof(paired_entry_t)){
        if(err != ESP_ERR_NVS_NOT_FOUND){
            CY_LOGW(TCP_SERVER_DB, "paired_list: stored blob unusable (%s, v%u, n=%u), starting empty",
                    esp_err_to_name(err), s_list.version, s_list.count);
        }
        memset(&s_list, 0, sizeof(s_list));
        s_list.version = BLOB_VERSION;
    }
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

/* Re-stamp every entry's role from owner_sub (called when owner_sub changes,
 * so the stored role stays authoritative for local_auth). Mutex held. */
static void recompute_roles_locked(void){
    for(int i = 0 ; i < s_list.count ; i++){
        s_list.entries[i].role = role_for_locked(s_list.entries[i].user_sub);
    }
}

static int find_locked(const char *user_sub, const char *install_id){
    for(int i = 0 ; i < s_list.count ; i++){
        if(strncmp(s_list.entries[i].user_sub, user_sub, PAIRED_SUB_MAX) == 0 &&
           strncmp(s_list.entries[i].install_id, install_id, PAIRED_INSTALL_MAX) == 0){
            return i;
        }
    }
    return -1;
}

/* ---- public ----------------------------------------------------------- */

void paired_list_init(void){
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    nvs_load();
    CY_LOGI(TCP_SERVER_DB, "paired_list: %u entr%s", s_list.count, s_list.count == 1 ? "y" : "ies");
}

int paired_list_count(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_list.count;
    xSemaphoreGive(s_mutex);
    return n;
}

int paired_list_find(const char *user_sub, const char *install_id){
    if(user_sub == NULL || install_id == NULL) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int i = find_locked(user_sub, install_id);
    xSemaphoreGive(s_mutex);
    return i;
}

int paired_list_find_other_install(const char *user_sub, const char *install_id){
    if(user_sub == NULL || install_id == NULL) return -1;
    int found = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for(int i = 0 ; i < s_list.count && found < 0 ; i++){
        if(strncmp(s_list.entries[i].user_sub, user_sub, PAIRED_SUB_MAX) == 0 &&
           strncmp(s_list.entries[i].install_id, install_id, PAIRED_INSTALL_MAX) != 0){
            found = i;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
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

int paired_list_add(const char *user_sub, const char *install_id,
                    const char *display_name, const uint8_t *key,
                    uint32_t paired_at, paired_role_t *role_out){
    if(user_sub == NULL || install_id == NULL || key == NULL) return -1;
    if(user_sub[0] == 0 || install_id[0] == 0) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* First phone to pair claims ownership for its account (bootstrap). */
    bool claimed_owner = false;
    if(s_list.owner_sub[0] == 0){
        copy_field(s_list.owner_sub, sizeof(s_list.owner_sub), user_sub);
        claimed_owner = true;
    }

    int i = find_locked(user_sub, install_id);
    paired_entry_t *e;
    bool appended = false;
    if(i >= 0){
        /* Same phone pairing again: fresh key. */
        e = &s_list.entries[i];
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
        copy_field(e->install_id, sizeof(e->install_id), install_id);
        s_list.count++;
        appended = true;
    }
    copy_field(e->display_name, sizeof(e->display_name), display_name);
    memcpy(e->key, key, PAIRED_KEY_LEN);
    e->paired_at = paired_at;
    /* Ownership is by account: role follows owner_sub, not insertion order. */
    e->role = role_for_locked(user_sub);

    esp_err_t err = nvs_store();
    if(err != ESP_OK){
        /* Undo so RAM and flash stay in step. */
        if(appended){
            memset(e, 0, sizeof(*e));
            s_list.count--;
        }
        if(claimed_owner) s_list.owner_sub[0] = 0;
        xSemaphoreGive(s_mutex);
        return -1;
    }
    if(role_out) *role_out = (paired_role_t)e->role;
    xSemaphoreGive(s_mutex);

    CY_LOGI(TCP_SERVER_DB, "paired_list: #%d %s (%s) role %u", i, e->user_sub, e->display_name, e->role);
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

esp_err_t paired_list_promote_owner(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if(s_list.count > 0){
        /* The owning account has phones as long as some entry matches owner_sub. */
        bool has_owner = false;
        for(int i = 0 ; i < s_list.count ; i++){
            if(strncmp(s_list.entries[i].user_sub, s_list.owner_sub, PAIRED_SUB_MAX) == 0){
                has_owner = true; break;
            }
        }
        if(!has_owner){
            /* Owner account is gone: hand ownership to the oldest remaining account. */
            copy_field(s_list.owner_sub, sizeof(s_list.owner_sub), s_list.entries[0].user_sub);
            recompute_roles_locked();
            err = nvs_store();
            CY_LOGI(TCP_SERVER_DB, "paired_list: owner_sub -> %s", s_list.owner_sub);
        }
    }
    xSemaphoreGive(s_mutex);
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
