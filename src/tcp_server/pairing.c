#include "pairing.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "device_config/device_config.h"
#include "wifi/wifi_config.h"
#include "common/cy_log.h"
#include "common/reset_handler.h"
#include "memory_handler/flash_wifi_info_handler.h"
#include "tcp_server/tcp_client_list.h"
#include "wifi/wifi_info_handler.h"
#include "device_config/cy_config.h"

#define START_PAIRING       "StartP"
#define FINISH_PAIRING      "FinishP"
#define PAIR_ACK_RESPONSE   "ACK"

#define START_PAIRING_IND   0
#define SET_ROUTER_SSID_IND 1
#define SET_ROUTER_PASS_IND 2
#define FINISH_PAIRING_IND  3

static int  pairing_step = START_PAIRING_IND;
static char temp_ssid[128] = "";
static char temp_password[128] = "";

/* ------------------------------------------------------------- timer --- */

/* Pairing inactivity watchdog: a one-shot FreeRTOS software timer. Every
 * accepted step re-arms it, FinishP stops it, and if it expires the state
 * machine is dropped back to START_PAIRING_IND so a fresh StartP is
 * accepted. The callback runs in the timer-service task; it only writes an
 * int. */
static TimerHandle_t pairing_timer = NULL;

static void pairing_timeout_cb(TimerHandle_t timer){
    if(pairing_step != START_PAIRING_IND){
        CY_LOGW(TCP_SERVER_DB, "pairing timed out at step %d, resetting", pairing_step);
        pairing_step = START_PAIRING_IND;
    }
}

void pairing_timer_init(void){
    pairing_timer = xTimerCreate("pairing", pdMS_TO_TICKS(PAIRING_TIMEOUT_MS),
                                 pdFALSE /* one-shot */, NULL, pairing_timeout_cb);
    configASSERT(pairing_timer != NULL);
}

/* Re-arm the watchdog: call after every accepted pairing step. */
static void pairing_touch(void){
    xTimerReset(pairing_timer, 0);
}

/* Disarm the watchdog: call when pairing finishes. */
static void pairing_done(void){
    xTimerStop(pairing_timer, 0);
}

/* ------------------------------------------------------------ parsing --- */

/* True if `line` starts with `prefix` and ends with `suffix` (an empty
 * suffix matches anything). Lines arrive with their trailing '\n' still
 * attached (tcp_client_recv_task NUL-terminates after it), so a "\n" suffix
 * is a real check. */
static bool is_pairing_command_valid(const char *line , const char *prefix , const char *suffix){

    size_t line_len   = strlen(line);
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);

    if(line_len < prefix_len + suffix_len){
        return false;
    }
    return memcmp(line , prefix , prefix_len) == 0
        && memcmp(line + line_len - suffix_len , suffix , suffix_len) == 0;
}

/* Copy the value between prefix and suffix of `line` into `out` (clamped to
 * out_size - 1, NUL-terminated). */
static void copy_pairing_value(const char *line , const char *prefix , const char *suffix ,
                              char *out , size_t out_size){
    int len = strlen(line) - strlen(prefix) - strlen(suffix);
    if(len >= (int)out_size){
        len = out_size - 1;
    }
    memcpy(out , line + strlen(prefix) , len);
    out[len] = 0;
}

/* ------------------------------------------------------------- commit --- */

esp_err_t pairing_commit(const char *ssid, const char *password){
    /* flash_store_wifi_router_info() wants the stored lengths INCLUDING the
     * NUL, so the strings read back terminated. */
    esp_err_t err = flash_store_wifi_router_info((char *)ssid, (char *)password, WIFI_STA_MODE,
                                                 strlen(ssid) + 1, strlen(password) + 1);
    if(err != ESP_OK){
        CY_LOGE(TCP_SERVER_DB, "storing Wi-Fi credentials failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 1 s for the caller's reply to leave, then reboot into station mode. */
    reboot_after_ms(1000);
    return ESP_OK;
}

/* ------------------------------------------------------ state machine --- */

bool pairing_handle_line(const char *line , int sock){

    if(memcmp(line , START_PAIRING , strlen(START_PAIRING)) == 0){
        /* StartP always (re)starts the flow, whatever step a previous
         * attempt got stuck at, so the app can retry without waiting for
         * the timeout. */
        pairing_step = SET_ROUTER_SSID_IND;
        pairing_touch();
        send_raw_to_client(sock, PAIR_ACK_RESPONSE);
        return true;
    }

    if(is_pairing_command_valid(line , cy_router_ssid_prefix , SET_ROUTER_SSID_SUFFIX)){
        if(pairing_step == SET_ROUTER_SSID_IND){
            copy_pairing_value(line , cy_router_ssid_prefix , SET_ROUTER_SSID_SUFFIX ,
                               temp_ssid , sizeof(temp_ssid));
            pairing_step = SET_ROUTER_PASS_IND;
            pairing_touch();
            send_raw_to_client(sock, PAIR_ACK_RESPONSE);
        }
        return true;
    }

    if(is_pairing_command_valid(line , cy_router_pass_prefix , SET_ROUTER_PASS_SUFFIX)){
        if(pairing_step == SET_ROUTER_PASS_IND){
            copy_pairing_value(line , cy_router_pass_prefix , SET_ROUTER_PASS_SUFFIX ,
                               temp_password , sizeof(temp_password));
            pairing_step = FINISH_PAIRING_IND;
            pairing_touch();
            send_raw_to_client(sock, PAIR_ACK_RESPONSE);
        }
        return true;
    }

    if(memcmp(line , FINISH_PAIRING , strlen(FINISH_PAIRING)) == 0){
        if(pairing_step == FINISH_PAIRING_IND){
            pairing_commit(temp_ssid , temp_password);
            pairing_step = START_PAIRING_IND;
            pairing_done();
            send_raw_to_client(sock, PAIR_ACK_RESPONSE);
        }
        return true;
    }

    return false;
}
