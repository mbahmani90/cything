#include "reset_handler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "memory_handler/flash_reset_handler.h"
#include "memory_handler/flash_wifi_info_handler.h"
#include "wifi/wifi_info_handler.h"
#include "security/device_password.h"
#include "security/paired_list.h"
#include "security/pairing_events.h"

static void reboot_cb(void *arg){
    (void)arg;
    esp_restart();
}

void reboot_after_ms(uint32_t ms){
    static esp_timer_handle_t reboot_timer = NULL;

    if(reboot_timer == NULL){
        const esp_timer_create_args_t args = {
            .callback = reboot_cb,
            .name     = "reboot",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &reboot_timer));
    }
    esp_timer_stop(reboot_timer);   /* ESP_ERR_INVALID_STATE if idle; harmless */
    ESP_ERROR_CHECK(esp_timer_start_once(reboot_timer, (uint64_t)ms * 1000));
}

void reset_handler(){
    
    if(flash_read_reset_counter() == ESP_OK){
        registeration_reset_counter++;

        if(registeration_reset_counter > 4){
            /* Factory reset by power cycling: the unit goes back to pairing
             * mode with no owner — whoever provisions it next sets the
             * password (doc/local-auth.md, "Threat model"). */
            paired_list_clear();
            device_password_clear();
            pairing_events_push("reset", "", "", "");
            flash_store_wifi_router_info("0" , "0" , (char *) WIFI_AP_MODE , 2 , 2);
            registeration_reset_counter = 0;
            flash_erase_write_reset_counter(registeration_reset_counter);
            esp_restart();            
        }else{
            flash_erase_write_reset_counter(registeration_reset_counter);
        }

    }

    vTaskDelay(2000 / portTICK_PERIOD_MS);
    registeration_reset_counter = 0;
    flash_erase_write_reset_counter(registeration_reset_counter);

}