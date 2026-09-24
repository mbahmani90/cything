#pragma once

/*
 * Line dispatcher for the TCP command server: hands each received line to
 * the module that owns its prefix (provisioning, HTTPS OTA handshake, Wi-Fi
 * pairing, local commands, then the application hook
 * app_command_handle_line() from CyThingEsp32.h). See doc/tcp-server.md
 * "command dispatch".
 */

typedef enum {
    UPDATE_FW_START,
    UPDATE_FW_CREDENTIAL,
    UPDATE_FW_UPDATING,
    UPDATE_FW_UPDATED
} UpdateFirmwareStep;

typedef struct {
    UpdateFirmwareStep step;
    int code;
    const char *command;
    const char *response;
} UpdateFirmwareStepInfo;

/* HTTPS OTA handshake strings, indexed by UpdateFirmwareStep. Also used by
 * ota_task to format its progress lines. */
extern UpdateFirmwareStepInfo updateFirmwareSteps[];

/* `line` is NUL-terminated with its trailing '\n' still attached; `len`
 * excludes the '\n'. `sock` is the client it came from. */
void tcp_dispatch_line(char *line , int len , int sock);
