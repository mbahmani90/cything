#pragma once

/* AWS IoT settings. */

/* 1: connect to AWS IoT after station bring-up and mirror broadcast responses
 * to the MQTT response topic; 0: local (TCP/UDP) only. */
#define IS_REMOTE_CON_ENABLE 1
