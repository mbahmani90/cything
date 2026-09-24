#include "pairing_events.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "common/cy_log.h"

#define NVS_NS        "cy_sec"
#define NVS_KEY_QUEUE "pevents"

typedef struct {
    uint8_t count;
    uint8_t reserved[3];
    char    json[PAIRING_EVENTS_MAX][PAIRING_EVENT_JSON_MAX];
} queue_t;

static queue_t           s_q;
static SemaphoreHandle_t s_mutex = NULL;

static void nvs_store(void){
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err;
    if(s_q.count == 0){
        err = nvs_erase_key(h, NVS_KEY_QUEUE);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }else{
        size_t n = offsetof(queue_t, json) + (size_t)s_q.count * PAIRING_EVENT_JSON_MAX;
        err = nvs_set_blob(h, NVS_KEY_QUEUE, &s_q, n);
    }
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if(err != ESP_OK){
        CY_LOGW(AWS_IOT_DB, "pairing_events: store failed: %s", esp_err_to_name(err));
    }
}

static void nvs_load(void){
    memset(&s_q, 0, sizeof(s_q));
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_q);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_QUEUE, &s_q, &n);
    nvs_close(h);
    if(err != ESP_OK || s_q.count > PAIRING_EVENTS_MAX ||
       n != offsetof(queue_t, json) + (size_t)s_q.count * PAIRING_EVENT_JSON_MAX){
        memset(&s_q, 0, sizeof(s_q));
    }
}

/* Minimal JSON string escaping for the identity fields (they are validated
 * printable ASCII on the way in, so only '"' and '\\' can need it). */
static void json_escape(const char *in, char *out, size_t out_size){
    size_t o = 0;
    for(const char *p = in ? in : "" ; *p && o + 2 < out_size ; p++){
        if(*p == '"' || *p == '\\'){
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

void pairing_events_init(void){
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
    nvs_load();
    if(s_q.count){
        CY_LOGI(AWS_IOT_DB, "pairing_events: %u queued from before reboot", s_q.count);
    }
}

void pairing_events_push(const char *event, const char *user_sub,
                         const char *install_id, const char *role){
    /* Bounded copies so the snprintf below is provably within
     * PAIRING_EVENT_JSON_MAX (52 literal + 15 + 80 + 64 + 15 + 10). */
    char ev[16], rl[16], sub[2 * 40 + 1], inst[2 * 32 + 1];
    json_escape(event, ev, sizeof(ev));
    json_escape(role, rl, sizeof(rl));
    json_escape(user_sub, sub, sizeof(sub));
    json_escape(install_id, inst, sizeof(inst));
    time_t t = time(NULL);
    unsigned long at = (t > 1600000000) ? (unsigned long)t : 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if(s_q.count >= PAIRING_EVENTS_MAX){
        /* Full: drop the oldest. */
        memmove(s_q.json[0], s_q.json[1], (size_t)(PAIRING_EVENTS_MAX - 1) * PAIRING_EVENT_JSON_MAX);
        s_q.count--;
    }
    snprintf(s_q.json[s_q.count], PAIRING_EVENT_JSON_MAX,
             "{\"event\":\"%s\",\"userSub\":\"%s\",\"installId\":\"%s\",\"role\":\"%s\",\"at\":%lu}",
             ev, sub, inst, rl, at);
    s_q.count++;
    nvs_store();
    xSemaphoreGive(s_mutex);

    CY_LOGI(AWS_IOT_DB, "pairing_events: queued %s (%u pending)", event ? event : "?", s_q.count);
}

int pairing_events_pending(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_q.count;
    xSemaphoreGive(s_mutex);
    return n;
}

bool pairing_events_peek(char *out, size_t out_size){
    bool ok = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if(s_q.count > 0){
        strncpy(out, s_q.json[0], out_size - 1);
        out[out_size - 1] = 0;
        ok = true;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void pairing_events_pop(void){
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if(s_q.count > 0){
        memmove(s_q.json[0], s_q.json[1], (size_t)(PAIRING_EVENTS_MAX - 1) * PAIRING_EVENT_JSON_MAX);
        s_q.count--;
        memset(s_q.json[s_q.count], 0, PAIRING_EVENT_JSON_MAX);
        nvs_store();
    }
    xSemaphoreGive(s_mutex);
}
