#ifndef CY_LOG_H
#define CY_LOG_H

#include "esp_log.h"
#include "device_config/device_config.h"   /* TAG */

/*
 * Per-module debug log switches: 1 = on, 0 = off.
 * Every switch must have a value (never an empty #define) because it is
 * evaluated as an expression inside CY_LOGx().
 */
#define TCP_SERVER_DB     1
#define UDP_DB            1
#define WIFI_STA_DB       1
#define AWS_IOT_DB        1
#define PROVISIONING_DB   0
#define TCP_OTA_DB        0
#define OTA_DB            0
#define FLASH_BOOT_DB     0
#define RESPONSE_FIFO_DB  1
#define BLE_DB            1

/*
 * CY_LOGx(<switch>, fmt, ...) — same signature as ESP_LOGx minus the tag.
 * When <switch> is 0 the compiler drops the whole branch, but the format
 * string and arguments are still type-checked. A misspelt switch is a
 * compile error rather than a silently disabled log.
 *
 * ESP_LOGx is itself a statement (do/while), so it cannot be wrapped in an
 * expression. The trailing `else ((void)0)` absorbs the caller's `;` and
 * keeps the macro safe as an unbraced `if`/`else` body.
 */
#define CY_LOGE(en, fmt, ...) if (en) { ESP_LOGE(TAG, fmt, ##__VA_ARGS__); } else ((void)0)
#define CY_LOGW(en, fmt, ...) if (en) { ESP_LOGW(TAG, fmt, ##__VA_ARGS__); } else ((void)0)
#define CY_LOGI(en, fmt, ...) if (en) { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); } else ((void)0)
#define CY_LOGD(en, fmt, ...) if (en) { ESP_LOGD(TAG, fmt, ##__VA_ARGS__); } else ((void)0)

#endif /* CY_LOG_H */
