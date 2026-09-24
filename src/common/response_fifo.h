#ifndef RESPONSE_FIFO_H
#define RESPONSE_FIFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Byte FIFO for outgoing responses, see doc/send-buffers.md.
 *
 * Producers (TCP client tasks, device routines) append newline-terminated
 * lines with response_fifo_push(). A consumer copies everything out with
 * response_fifo_drain() and then does the (slow) network send outside the
 * lock. This file is the generic engine only; the two instances live with
 * their consumers: tcp_server/tcp_response_fifo.[ch] (drained immediately
 * by tcp_response_send_task) and aws/mqtt_response_fifo.[ch] (drained
 * periodically by aws_iot_task).
 */

#define RESPONSE_FIFO_SIZE              5120

/* What push() does when the line does not fit. */
#define RESPONSE_FIFO_DROP_NEW          0   /* reject the new line, keep the old ones */
#define RESPONSE_FIFO_OVERWRITE_OLDEST  1   /* discard whole lines from the head until it fits */
#ifndef RESPONSE_FIFO_ON_FULL
#define RESPONSE_FIFO_ON_FULL           RESPONSE_FIFO_DROP_NEW
#endif

typedef struct {
    char              buf[RESPONSE_FIFO_SIZE];
    size_t            len;         /* index of the first free byte */
    SemaphoreHandle_t mutex;       /* only one task touches buf/len at a time */
    SemaphoreHandle_t data_ready;  /* binary; given after every successful push */
    const char       *name;        /* for logs */
} response_fifo_t;

/* Create the mutex/semaphore and clear the buffer. Call once per instance
 * before any producer task exists. */
void response_fifo_init(response_fifo_t *f, const char *name);

/* Append `len` bytes of `data`; a trailing '\n' is added if missing.
 * Returns false if the line was dropped (RESPONSE_FIFO_DROP_NEW and full,
 * or the line is larger than the whole FIFO). */
bool response_fifo_push(response_fifo_t *f, const char *data, size_t len);

/* Copy up to `out_cap` bytes to `out`, remove them from the FIFO and clear
 * the freed bytes. Returns the number of bytes copied. `out` is NOT
 * NUL-terminated; size `out` as RESPONSE_FIFO_SIZE + 1 and terminate it
 * yourself if you need a C string. */
size_t response_fifo_drain(response_fifo_t *f, char *out, size_t out_cap);

/* Block until the FIFO is non-empty or `ticks` elapse. Returns true if
 * there is data to drain. */
bool response_fifo_wait(response_fifo_t *f, TickType_t ticks);

bool response_fifo_is_empty(response_fifo_t *f);

#endif /* RESPONSE_FIFO_H */
