#pragma once

#include <stdint.h>
#include "common/response_fifo.h"

/*
 * The remote (MQTT) response FIFO. Producer: send_data_to_clients() for
 * SEND_TO_ALL broadcasts when IS_REMOTE_CON_ENABLE. Consumer: aws_iot_task,
 * which drains it every mqtt_response_get_period_ms() and publishes the
 * batch to the device's response topic. Lines are "<id>:<data>\n"; see
 * doc/send-buffers.md.
 */

extern response_fifo_t mqtt_response_fifo;

/* Call once from app_main() before any producer task starts. */
void mqtt_response_fifo_init(void);

/* Drain period, clamped to [MIN, MAX]. */
#define MQTT_RESPONSE_PERIOD_MS_DEFAULT 1000U
#define MQTT_RESPONSE_PERIOD_MS_MIN     100U
#define MQTT_RESPONSE_PERIOD_MS_MAX     3600000U

void     mqtt_response_set_period_ms(uint32_t period_ms);
uint32_t mqtt_response_get_period_ms(void);
