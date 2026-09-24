#ifndef COREMQTT_CONFIG_H
#define COREMQTT_CONFIG_H

#include "sdkconfig.h"

/* Under ESP-IDF these come from menuconfig (main/Kconfig.projbuild, "coreMQTT").
 * The precompiled Arduino core's sdkconfig.h has none of them, so fall back
 * to the same defaults the Kconfig declares. */
#ifndef CONFIG_MQTT_STATE_ARRAY_MAX_COUNT
    #define CONFIG_MQTT_STATE_ARRAY_MAX_COUNT             10
#endif
#ifndef CONFIG_MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT
    #define CONFIG_MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT   5
#endif
#ifndef CONFIG_MQTT_PINGRESP_TIMEOUT_MS
    #define CONFIG_MQTT_PINGRESP_TIMEOUT_MS               5000
#endif
#ifndef CONFIG_MQTT_RECV_POLLING_TIMEOUT_MS
    #define CONFIG_MQTT_RECV_POLLING_TIMEOUT_MS           10
#endif
#ifndef CONFIG_MQTT_SEND_TIMEOUT_MS
    #define CONFIG_MQTT_SEND_TIMEOUT_MS                   20000
#endif
/* Kconfig bools are simply absent when "n", so "all four absent" cannot be
 * told from "all four off" — only apply the error+info default on Arduino,
 * where there is no Kconfig at all. */
#if defined(ARDUINO) && !defined(CONFIG_CORE_MQTT_LOG_ERROR) && !defined(CONFIG_CORE_MQTT_LOG_WARN) \
    && !defined(CONFIG_CORE_MQTT_LOG_INFO) && !defined(CONFIG_CORE_MQTT_LOG_DEBUG)
    #define CONFIG_CORE_MQTT_LOG_ERROR 1
    #define CONFIG_CORE_MQTT_LOG_INFO  1
#endif

#define EXTRACT_ARGS( ... ) __VA_ARGS__
#define STRIP_PARENS( X ) X
#define REMOVE_PARENS( X ) STRIP_PARENS( EXTRACT_ARGS X )

/* Logging configurations */
#if CONFIG_CORE_MQTT_LOG_ERROR || CONFIG_CORE_MQTT_LOG_WARN || CONFIG_CORE_MQTT_LOG_INFO || CONFIG_CORE_MQTT_LOG_DEBUG

    /* Set logging level for the coreMQTT and coreMQTT-Agent components to highest level,
     * so any defined logging level below is printed. */
    #ifdef LOG_LOCAL_LEVEL
        #undef LOG_LOCAL_LEVEL
    #endif
    #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
    #include "esp_log.h"

    /* Change LIBRARY_LOG_NAME to "coreMQTT" if defined somewhere else. */
    #ifdef LIBRARY_LOG_NAME
        #undef LIBRARY_LOG_NAME
    #endif
    #define LIBRARY_LOG_NAME "coreMQTT"

#endif

/* Undefine logging macros if they were defined somewhere else like another AWS/FreeRTOS library. */
#ifdef LogError
    #undef LogError
#endif

#ifdef LogWarn
    #undef LogWarn
#endif

#ifdef LogInfo
    #undef LogInfo
#endif

#ifdef LogDebug
    #undef LogDebug
#endif

/* Define logging macros based on configurations in sdkconfig.h. */
#if CONFIG_CORE_MQTT_LOG_ERROR
    #define LogError( message, ... ) ESP_LOGE( LIBRARY_LOG_NAME, REMOVE_PARENS( message ), ##__VA_ARGS__ )
#else
    #define LogError( message, ... )
#endif

#if CONFIG_CORE_MQTT_LOG_WARN
    #define LogWarn( message, ... ) ESP_LOGW( LIBRARY_LOG_NAME, REMOVE_PARENS( message ), ##__VA_ARGS__ )
#else
    #define LogWarn( message, ... )
#endif

#if CONFIG_CORE_MQTT_LOG_INFO
    #define LogInfo( message, ... ) ESP_LOGI( LIBRARY_LOG_NAME, REMOVE_PARENS( message ), ##__VA_ARGS__ )
#else
    #define LogInfo( message, ... )
#endif

#if CONFIG_CORE_MQTT_LOG_DEBUG
    #define LogDebug( message, ... ) ESP_LOGD( LIBRARY_LOG_NAME, REMOVE_PARENS( message ), ##__VA_ARGS__ )
#else
    #define LogDebug( message, ... )
#endif

/* coreMQTT configurations */
#define MQTT_STATE_ARRAY_MAX_COUNT CONFIG_MQTT_STATE_ARRAY_MAX_COUNT
#define MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT CONFIG_MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT
#define MQTT_PINGRESP_TIMEOUT_MS CONFIG_MQTT_PINGRESP_TIMEOUT_MS
#define MQTT_RECV_POLLING_TIMEOUT_MS CONFIG_MQTT_RECV_POLLING_TIMEOUT_MS
#define MQTT_SEND_TIMEOUT_MS CONFIG_MQTT_SEND_TIMEOUT_MS

#endif /* COREMQTT_CONFIG_H */