#include "tcp_client_list.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "aws/aws_config.h"
#include "common/cy_log.h"
#include "common/response_fifo.h"
#include "tcp_server/tcp_response_fifo.h"
#include "aws/mqtt_response_fifo.h"
#include "security/enc_frame.h"
#include "security/access_policy.h"
#include "security/device_password.h"
#include "device_config/device_config.h"

struct account_struct account_struct_list[100];
int   account_list_size = 0;
int   response_id = 10;
static SemaphoreHandle_t response_id_mutex = NULL;

/* Call once from app_main() before any task can produce a response. */
void tcp_client_list_init(){
    response_id_mutex = xSemaphoreCreateMutex();
    configASSERT(response_id_mutex != NULL);
}

/* Advance the shared response id and return the new value. Locked because
 * every TCP client task and aws_iot_task call this concurrently; use the
 * returned value, not the global, so two callers never get the same id. */
int update_response_id(){
    xSemaphoreTake(response_id_mutex, portMAX_DELAY);
    response_id++;
    if(response_id >= 255){
        response_id = 10;
    }
    int id = response_id;
    xSemaphoreGive(response_id_mutex);
    return id;
}

void remove_account(int sock) {
    
    int temp_index = -1;

    for(int i = 0 ; i < account_list_size ; i++){
        if(account_struct_list[i].sock == sock){
            temp_index = i;
            break;
        }
    }

    if(temp_index >= 0){
        for (int i = temp_index; i < account_list_size - 1; i++) {
            account_struct_list[i] = account_struct_list[i + 1];
        }
        account_list_size--;
    }

    
}

bool add_account(int sock){
    
    if(account_list_size >= 100) return false;

    account_struct_list[account_list_size].sock = sock;
    account_list_size++;
    return true;
}

static bool is_client_connected(int sock){
    for(int i = 0 ; i < account_list_size ; i++){
        if(account_struct_list[i].sock == sock) return true;
    }
    return false;
}

/* Push one "<sock>:<payload>" line on the TCP FIFO. The send task strips the
 * "<sock>:" prefix and forwards the payload verbatim, so whatever is after the
 * prefix is exactly what the client receives. If the line has to be
 * truncated, the trailing '\n' is kept so it can't glue onto the next line
 * (the send task splits on '\n'). Returns the stored line via `line_out` for
 * callers that also mirror it (MQTT). */
static int push_tcp_line(char *line_out, const char *fmt, ...){

    va_list ap;
    va_start(ap, fmt);
    int line_size = vsnprintf(line_out, RESPONSE_LINE_MAX, fmt, ap);
    va_end(ap);

    if(line_size >= RESPONSE_LINE_MAX){
        line_size = RESPONSE_LINE_MAX - 1;
        line_out[line_size - 1] = '\n';
    }
    response_fifo_push(&tcp_response_fifo, line_out, line_size);
    return line_size;
}

/* Enqueue one application response. The TCP FIFO line is "<sock>:<id>:<data>\n"
 * where <sock> is the target socket or RESPONSE_TARGET_ALL (0) for every
 * client. The MQTT FIFO line is "<id>:<data>\n" and is only fed for
 * broadcasts, as before. `data` must already end with '\n'. */
void send_data_to_clients(int sock , const char *data , int data_size){

    char line[RESPONSE_LINE_MAX];
    int  target = (sock == SEND_TO_ALL)        ? RESPONSE_TARGET_ALL :
                  (sock == SEND_TO_ALL_PUBLIC) ? RESPONSE_TARGET_EVERYONE : sock;

    int id = update_response_id();
    int line_size = push_tcp_line(line, "%d:%d:%.*s", target, id, data_size, data);

#if IS_REMOTE_CON_ENABLE == 1
    if(sock == SEND_TO_ALL || sock == SEND_TO_ALL_PUBLIC){
        /* Skip the "<sock>:" prefix: MQTT gets "<id>:<data>\n". */
        char *mqtt_line = strchr(line, ':') + 1;
        response_fifo_push(&mqtt_response_fifo, mqtt_line, line_size - (mqtt_line - line));
    }
#endif

}

/* Enqueue one protocol reply — pairing ACKs, OTA progress, … — for a single
 * socket, with no response id and no MQTT mirror: the client receives
 * "<text>\n" exactly. Goes through the same FIFO as send_data_to_clients so
 * every byte written to a client leaves from tcp_response_send_task, in
 * order, and a reply to a client that has already gone is dropped there
 * instead of being send()-ed to a stale descriptor. */
void send_raw_to_client(int sock , const char *text){

    char line[RESPONSE_LINE_MAX];
    push_tcp_line(line, "%d:%s\n", sock, text);

}

/* Local send task: wakes on every push, copies the whole TCP FIFO out
 * (releasing the lock) and routes each line to its target socket(s). */
void tcp_response_send_task(void *pvParameters){

    static char staging[RESPONSE_FIFO_SIZE];

    for( ; ; ){
        response_fifo_wait(&tcp_response_fifo, portMAX_DELAY);

        size_t n = response_fifo_drain(&tcp_response_fifo, staging, sizeof(staging));

        char *line = staging;
        char *end  = staging + n;
        while(line < end){
            char *nl = memchr(line, '\n', end - line);
            char *line_end = (nl != NULL) ? nl + 1 : end;   /* include the '\n' */

            /* Parse "<sock>:" prefix. */
            char *sep = memchr(line, ':', line_end - line);
            char *endptr = NULL;
            long  target = (sep != NULL) ? strtol(line, &endptr, 10) : -1;
            if(sep == NULL || endptr != sep || sep == line){
                CY_LOGW(RESPONSE_FIFO_DB, "tcp_response: line without target prefix dropped: %.*s",
                        (int)(line_end - line), line);
                line = line_end;
                continue;
            }

            char  *payload      = sep + 1;
            size_t payload_size = line_end - payload;

            /* enc_frame_send() encrypts for sockets that hold a session key
             * and passes plaintext sockets straight to send(). */
            if(target == RESPONSE_TARGET_ALL || target == RESPONSE_TARGET_EVERYONE){
                for(int i = 0 ; i < account_list_size ; i++){
                    int dst = account_struct_list[i].sock;
#if LOCAL_AUTH_ENFORCE == 1
                    /* A paired phone's data does not go to a socket that is
                     * only here for a public command — but only while the device
                     * HAS a password. With no password the device is open: nobody
                     * can authenticate (there is no verifier to PAKE against), so
                     * every socket is legitimate and gets the broadcast. This
                     * mirrors access_policy's is_public no-password shortcut, which
                     * already ACCEPTS these commands; without it the command runs
                     * but its response is dropped for the open client. */
                    if(target == RESPONSE_TARGET_ALL && device_password_is_set() &&
                       !access_policy_sock_authenticated(dst)) continue;
#endif
                    enc_frame_send(dst, payload, payload_size);
                }
            }else if(is_client_connected((int)target)){
                enc_frame_send((int)target, payload, payload_size);
            }else{
                CY_LOGW(RESPONSE_FIFO_DB, "tcp_response: socket %ld gone, dropped: %.*s",
                        target, (int)payload_size, payload);
            }

            line = line_end;
        }
    }

}
