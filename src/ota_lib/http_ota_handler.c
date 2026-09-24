#include "http_ota_handler.h"

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_https_ota.h"

#include "device_config/device_config.h"
#include "aws/root_cert_auth.h"
#include "common/reset_handler.h"
#include "tcp_server/tcp_client_list.h"
#include "tcp_server/tcp_command.h"

char firmwareUrl[4096] = "";

void ota_task(void *pvParameter){

    ESP_LOGI(TAG, "Starting OTA update...");

    int sock = (int)(intptr_t)pvParameter;

    esp_http_client_config_t http_config = {
        .url = firmwareUrl,
        .cert_pem = root_cert_auth_pem,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    
    // esp_err_t ret = esp_https_ota(&ota_config);
    // if (ret == ESP_OK) {
    //     esp_restart();
    // } else {
    //     ESP_LOGE("OTA", "Firmware upgrade failed");
    // }

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA Begin failed");
        return;
    }

    // Get total size (Content-Length from HTTP headers)
    int total_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "Firmware size: %d bytes", total_size);

    int last_percent = -1;
    while (1) {
        ret = esp_https_ota_perform(https_ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        // Get how much has been read
        int bytes_read = esp_https_ota_get_image_len_read(https_ota_handle);

        if (total_size > 0) {
            int percent = (bytes_read * 100) / total_size;
            if (percent != last_percent) {
                ESP_LOGI(TAG, "Downloaded %d%%", percent);
                char progress[32];
                snprintf(progress, sizeof(progress), "%s%d", updateFirmwareSteps[UPDATE_FW_UPDATING].response, percent);
                send_raw_to_client(sock, progress);
                last_percent = percent;
            }
        } else {
            ESP_LOGI(TAG, "Downloaded %d bytes", bytes_read);
        }
    }

    if (esp_https_ota_is_complete_data_received(https_ota_handle) == true) {
        ESP_LOGI(TAG, "OTA Success, restarting...");
        esp_https_ota_finish(https_ota_handle);
        send_raw_to_client(sock, updateFirmwareSteps[UPDATE_FW_UPDATED].response);
        reboot_after_ms(2000);
    } else {
        ESP_LOGE(TAG, "OTA Failed");
        esp_https_ota_abort(https_ota_handle);
}

    vTaskDelete(NULL);
}