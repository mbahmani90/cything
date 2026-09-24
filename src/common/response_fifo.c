#include "response_fifo.h"

#include <string.h>
#include "common/cy_log.h"

void response_fifo_init(response_fifo_t *f, const char *name){
    memset(f->buf, 0, sizeof(f->buf));
    f->len        = 0;
    f->name       = name;
    f->mutex      = xSemaphoreCreateMutex();
    f->data_ready = xSemaphoreCreateBinary();
    configASSERT(f->mutex != NULL && f->data_ready != NULL);
}

#if RESPONSE_FIFO_ON_FULL == RESPONSE_FIFO_OVERWRITE_OLDEST
/* Discard whole lines from the head until at least `need` bytes are free.
 * Caller holds the mutex. */
static void discard_oldest(response_fifo_t *f, size_t need){
    size_t discard = 0;
    while(RESPONSE_FIFO_SIZE - (f->len - discard) < need && discard < f->len){
        char *nl = memchr(f->buf + discard, '\n', f->len - discard);
        discard = (nl != NULL) ? (size_t)(nl - f->buf) + 1 : f->len;
    }
    if(discard > 0){
        memmove(f->buf, f->buf + discard, f->len - discard);
        f->len -= discard;
        memset(f->buf + f->len, 0, discard);
        CY_LOGW(RESPONSE_FIFO_DB, "%s full: discarded %u oldest bytes", f->name, (unsigned)discard);
    }
}
#endif

bool response_fifo_push(response_fifo_t *f, const char *data, size_t len){
    bool   add_nl = (len == 0 || data[len - 1] != '\n');
    size_t need   = len + (add_nl ? 1 : 0);

    if(need > RESPONSE_FIFO_SIZE){
        CY_LOGW(RESPONSE_FIFO_DB, "%s: line of %u bytes larger than FIFO, dropped", f->name, (unsigned)need);
        return false;
    }

    xSemaphoreTake(f->mutex, portMAX_DELAY);

    if(f->len + need > RESPONSE_FIFO_SIZE){
#if RESPONSE_FIFO_ON_FULL == RESPONSE_FIFO_OVERWRITE_OLDEST
        discard_oldest(f, need);
#else
        xSemaphoreGive(f->mutex);
        CY_LOGW(RESPONSE_FIFO_DB, "%s full (%u/%u), dropped %u bytes",
                f->name, (unsigned)f->len, RESPONSE_FIFO_SIZE, (unsigned)need);
        return false;
#endif
    }

    memcpy(f->buf + f->len, data, len);
    f->len += len;
    if(add_nl){
        f->buf[f->len++] = '\n';
    }

    xSemaphoreGive(f->mutex);
    xSemaphoreGive(f->data_ready);
    return true;
}

size_t response_fifo_drain(response_fifo_t *f, char *out, size_t out_cap){
    xSemaphoreTake(f->mutex, portMAX_DELAY);

    size_t n = (f->len < out_cap) ? f->len : out_cap;
    memcpy(out, f->buf, n);

    /* Normally n == len and this is a plain clear; if the caller's buffer is
     * smaller, keep the remainder at the head for the next drain. */
    f->len -= n;
    memmove(f->buf, f->buf + n, f->len);
    memset(f->buf + f->len, 0, n);

    xSemaphoreGive(f->mutex);
    return n;
}

bool response_fifo_wait(response_fifo_t *f, TickType_t ticks){
    if(!response_fifo_is_empty(f)){
        return true;
    }
    return xSemaphoreTake(f->data_ready, ticks) == pdTRUE;
}

bool response_fifo_is_empty(response_fifo_t *f){
    xSemaphoreTake(f->mutex, portMAX_DELAY);
    bool empty = (f->len == 0);
    xSemaphoreGive(f->mutex);
    return empty;
}
