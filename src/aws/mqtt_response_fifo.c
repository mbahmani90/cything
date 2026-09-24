#include "mqtt_response_fifo.h"

#include "common/cy_log.h"

response_fifo_t mqtt_response_fifo;

static volatile uint32_t mqtt_response_period_ms = MQTT_RESPONSE_PERIOD_MS_DEFAULT;

void mqtt_response_fifo_init(void){
    response_fifo_init(&mqtt_response_fifo, "mqtt_response");
}

void mqtt_response_set_period_ms(uint32_t period_ms){
    if(period_ms < MQTT_RESPONSE_PERIOD_MS_MIN) period_ms = MQTT_RESPONSE_PERIOD_MS_MIN;
    if(period_ms > MQTT_RESPONSE_PERIOD_MS_MAX) period_ms = MQTT_RESPONSE_PERIOD_MS_MAX;
    mqtt_response_period_ms = period_ms;
    CY_LOGI(RESPONSE_FIFO_DB, "mqtt_response period set to %u ms", (unsigned)period_ms);
}

uint32_t mqtt_response_get_period_ms(void){
    return mqtt_response_period_ms;
}
