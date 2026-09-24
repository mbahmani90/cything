#include "tcp_command.h"
#include "CyThingEsp32.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common/cy_log.h"
#include "common/task_config.h"
#include "aws/provisioning.h"
#include "ota_lib/http_ota_handler.h"
#include "tcp_server/pairing.h"
#include "tcp_server/tcp_client_list.h"
#include "security/pake_handler.h"
#include "security/local_auth.h"
#include "security/access_policy.h"
#include "security/owner_commands.h"

UpdateFirmwareStepInfo updateFirmwareSteps[] = {
    { UPDATE_FW_START,      0, "startupdatecomm", "startupdateres" },
    { UPDATE_FW_CREDENTIAL, 1, "credentialcomm",  "credentialres"  },
    { UPDATE_FW_UPDATING,   2, "updatingcomm",    "updating:"      },
    { UPDATE_FW_UPDATED,    3, "updatedcomm",     "updatedres"     }
};

/* HTTPS OTA handshake: startupdatecomm -> credentialcomm -> updatingcomm<url>.
 * Returns true if `line` was one of these. */
static bool ota_handle_line(char *line , int len , int sock){

    if(memcmp(line , updateFirmwareSteps[UPDATE_FW_START].command , strlen(updateFirmwareSteps[UPDATE_FW_START].command)) == 0){
        send_raw_to_client(sock, updateFirmwareSteps[UPDATE_FW_START].response);
        return true;
    }
    if(memcmp(line , updateFirmwareSteps[UPDATE_FW_CREDENTIAL].command , strlen(updateFirmwareSteps[UPDATE_FW_CREDENTIAL].command)) == 0){
        send_raw_to_client(sock, updateFirmwareSteps[UPDATE_FW_CREDENTIAL].response);
        return true;
    }
    if(memcmp(line , updateFirmwareSteps[UPDATE_FW_UPDATING].command , strlen(updateFirmwareSteps[UPDATE_FW_UPDATING].command)) == 0){
        int cmd_len = strlen(updateFirmwareSteps[UPDATE_FW_UPDATING].command);
        int url_len = len - cmd_len;
        if(url_len >= (int)sizeof(firmwareUrl)){
            url_len = sizeof(firmwareUrl) - 1;
        }
        memcpy(firmwareUrl , line + cmd_len , url_len);
        firmwareUrl[url_len] = 0;
        /* Pass the socket by value: this returns before ota_task
         * necessarily runs, so a pointer to `sock` would dangle. */
        xTaskCreate(&ota_task, "ota_task", OTA_TASK_STACK_SIZE, (void *)(intptr_t)sock, OTA_TASK_PRIORITY, NULL);
        char updating_response[32];
        snprintf(updating_response, sizeof(updating_response), "%s2", updateFirmwareSteps[UPDATE_FW_UPDATING].response);
        send_raw_to_client(sock, updating_response);
        return true;
    }
    return false;
}

/* Fallback for app_command_handle_line(); overridden by the definition in
 * app_main.c. */
__attribute__((weak)) bool app_command_handle_line(const char *line , int len , int sock){
    (void)line; (void)len; (void)sock;
    return false;
}

void tcp_dispatch_line(char *line , int len , int sock){

    CY_LOGI(TCP_SERVER_DB, "%s" , line);

    // Access policy: does this line need a paired phone behind it, and is
    // there one? Sends ERR:AUTH / ERR:ENC itself when not.
    if(!access_policy_allow(line, len, sock)){
        return;
    }
    // Password pairing (PAKE1/PAKE3) — see security/pake_handler.h.
    if(pake_handle_line(line, len, sock)){
        return;
    }
    // Enrollment + reconnect auth (ENROLL / AUTH1 / AUTH3) — security/local_auth.h.
    if(local_auth_handle_line(line, len, sock)){
        return;
    }
    // Paired-list / password management (LIST / REVOKE / PWSET / RESET) —
    // security/owner_commands.h. Owner-only, checked inside.
    if(owner_commands_handle_line(line, len, sock)){
        return;
    }
    // Per-device-cert provisioning handshake (PROV:/CSRREQ/DEVID:/CERT:/PFIN) —
    // see aws/provisioning.h. Checked first since it owns a handful of
    // otherwise-unused line prefixes.
    if(provisioning_handle_tcp_line(line, len, sock)){
        return;
    }
    if(ota_handle_line(line, len, sock)){
        return;
    }
    if(pairing_handle_line(line, sock)){
        return;
    }
    /* Device commands belong to the application, not the library: everything
     * above this point is protocol (auth, pairing, provisioning, OTA), and
     * anything else is the sketch's to define. */
    if(app_command_handle_line(line, len, sock)){
        return;
    }
    CY_LOGW(TCP_SERVER_DB, "unrecognised line ignored: %.*s", len, line);
}
