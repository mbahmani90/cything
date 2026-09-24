#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Pairing events for the backend (doc/local-auth.md, "Remote access from
 * the same pairing"). Local access is decided here on the device; remote
 * (MQTT) access is decided by the backend, and this is how it learns what
 * happened: after ENROLL / REVOKE / RESET the device publishes
 *
 *   topic    <sourceTerminalId>/<deviceType>/<deviceId>/pairing
 *   payload  {"event":"paired"|"unpaired"|"reset","userSub":"…","installId":"…","role":"owner"|"user","at":<unix s>}
 *
 * over its own mutual-TLS MQTT connection, so the backend can trust that
 * it is genuine — a phone cannot forge it. Events are queued in NVS until
 * the device is connected, so a pairing done while offline reaches the
 * cloud on the next connect, and a reboot in between loses nothing. The
 * queue is small (PAIRING_EVENTS_MAX); when full the oldest event is
 * dropped — the backend can always reconcile from a LIST later.
 *
 * Publishing is done by aws_iot_task from its process loop
 * (peek/pop around publishToTopic), never from the TCP tasks.
 */

#define PAIRING_EVENTS_MAX      8
#define PAIRING_EVENT_JSON_MAX  256

/* Load the queue from NVS. Call once from cything_begin() after NVS is up. */
void pairing_events_init(void);

/* Queue one event. Strings may be NULL/empty (RESET carries no identity). */
void pairing_events_push(const char *event, const char *user_sub,
                         const char *install_id, const char *role);

/* Number of queued events. */
int  pairing_events_pending(void);

/* Copy the oldest queued event's JSON into `out` (PAIRING_EVENT_JSON_MAX).
 * False if the queue is empty. */
bool pairing_events_peek(char *out, size_t out_size);

/* Drop the oldest event (after a successful publish). */
void pairing_events_pop(void);
