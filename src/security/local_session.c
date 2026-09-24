#include "local_session.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_srp.h"

#include "common/cy_log.h"

static local_session_t   pool[LOCAL_SESSION_MAX];
static SemaphoreHandle_t pool_mutex = NULL;

/* Wipe everything that must not survive the connection. Called with the
 * mutex held. */
static void wipe(local_session_t *s){
    if(s->srp != NULL){
        esp_srp_free((esp_srp_handle_t *)s->srp);
        s->srp = NULL;
    }
    /* memset on key material: mark volatile so the compiler cannot drop
     * the store as a dead write before the slot is reused. */
    volatile uint8_t *k = s->key;
    for(size_t i = 0 ; i < sizeof(s->key) ; i++) k[i] = 0;
    memset(s->nonce_dev,   0, sizeof(s->nonce_dev));
    memset(s->nonce_phone, 0, sizeof(s->nonce_phone));
    s->tx_ctr = s->rx_ctr = 0;
    s->rx_encrypted = false;
    s->state = LS_NEW;
    s->role  = LS_ROLE_NONE;
    s->paired_index = -1;
    s->rx_len = 0;
    s->peer_ip = 0;
    s->bootstrap_claim = false;
    s->sock = -1;
}

void local_session_init(void){
    pool_mutex = xSemaphoreCreateMutex();
    configASSERT(pool_mutex != NULL);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        memset(&pool[i], 0, sizeof(pool[i]));
        pool[i].sock = -1;
        pool[i].paired_index = -1;
    }
}

local_session_t *local_session_acquire(int sock, uint32_t peer_ip){
    local_session_t *s = NULL;
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock == -1){
            s = &pool[i];
            wipe(s);
            s->sock = sock;
            s->peer_ip = peer_ip;
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
    if(s == NULL){
        CY_LOGW(TCP_SERVER_DB, "local_session: pool full (%d), sock %d refused", LOCAL_SESSION_MAX, sock);
    }
    return s;
}

local_session_t *local_session_find(int sock){
    if(sock < 0) return NULL;
    local_session_t *s = NULL;
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock == sock){
            s = &pool[i];
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
    return s;
}

void local_session_release(int sock){
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock == sock){
            wipe(&pool[i]);
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
}

/* Mutex held. Forget the keys and the entry, keep the socket. */
static void drop_auth(local_session_t *s){
    if(s->srp != NULL){
        esp_srp_free((esp_srp_handle_t *)s->srp);
        s->srp = NULL;
    }
    volatile uint8_t *k = s->key;
    for(size_t i = 0 ; i < sizeof(s->key) ; i++) k[i] = 0;
    s->tx_ctr = s->rx_ctr = 0;
    s->state = LS_NEW;
    s->role  = LS_ROLE_NONE;
    s->paired_index = -1;
}

void local_session_on_paired_removed(int index){
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock == -1) continue;
        if(pool[i].paired_index == index){
            CY_LOGI(TCP_SERVER_DB, "local_session: sock %d revoked, auth dropped", pool[i].sock);
            drop_auth(&pool[i]);
        }else if(pool[i].paired_index > index){
            pool[i].paired_index--;
        }
    }
    xSemaphoreGive(pool_mutex);
}

void local_session_reset_handshake(local_session_t *s){
    if(s == NULL) return;
    if(s->srp != NULL){
        esp_srp_free((esp_srp_handle_t *)s->srp);
        s->srp = NULL;
    }
    volatile uint8_t *k = s->key;
    for(size_t i = 0 ; i < sizeof(s->key) ; i++) k[i] = 0;
    memset(s->nonce_dev,   0, sizeof(s->nonce_dev));
    memset(s->nonce_phone, 0, sizeof(s->nonce_phone));
    s->tx_ctr = s->rx_ctr = 0;
    s->rx_encrypted = false;
    s->state = LS_NEW;
    s->role  = LS_ROLE_NONE;
    s->paired_index = -1;
}

void local_session_drop_all_auth(void){
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock != -1) drop_auth(&pool[i]);
    }
    xSemaphoreGive(pool_mutex);
}

int local_session_count(void){
    int n = 0;
    xSemaphoreTake(pool_mutex, portMAX_DELAY);
    for(int i = 0 ; i < LOCAL_SESSION_MAX ; i++){
        if(pool[i].sock != -1) n++;
    }
    xSemaphoreGive(pool_mutex);
    return n;
}
