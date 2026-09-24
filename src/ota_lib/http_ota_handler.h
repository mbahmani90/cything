#pragma once

/* HTTPS OTA: ota_task downloads firmwareUrl, streams "updating:<pct>" lines
 * to the TCP socket it was started for, sends "updatedres" and reboots. */

extern char firmwareUrl[4096];

/* pvParameter: the client socket, passed by value as (void *)(intptr_t)sock. */
void ota_task(void *pvParameter);
