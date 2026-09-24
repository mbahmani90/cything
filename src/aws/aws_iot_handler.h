#pragma once

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "demo_config.h"
#include "coreMQTT/core_mqtt.h"
#include "coreMQTT/core_mqtt_state.h"
#include "coreMQTT/network_transport.h"
#include "backoffAlgorithm/backoff_algorithm.h"
#include "posix_compat/clock.h"
#include "device_config/device_config.h"

#undef DB_AWS_IOT

#define SUBSCRIBE_TOPICS_SIZE 3

/* The AWS IoT message broker requires either a set of client certificate/private key
 * or username/password to authenticate the client. */
#ifdef CLIENT_USERNAME
/* If a username is defined, a client password also would need to be defined for
 * client authentication. */
    #ifndef CLIENT_PASSWORD
        #error "Please define client password(CLIENT_PASSWORD) in demo_config.h for client authentication based on username/password."
    #endif
/* AWS IoT MQTT broker port needs to be 443 for client authentication based on
 * username/password. */
    #if AWS_MQTT_PORT != 443
        #error "Broker port, AWS_MQTT_PORT, should be defined as 443 in demo_config.h for client authentication based on username/password."
    #endif
#else /* !CLIENT_USERNAME */
/* The client cert/key come from the per-device identity minted by the CSR
 * handshake (g_mqtt_identity, aws/provisioning.c) — nothing is embedded. */
#endif /* CLIENT_USERNAME */

extern MQTTContext_t mqttContext;

/*
 * The identity used to connect — the per-device one from the CSR handshake
 * (client id = deviceId, its own cert + key, endpoint from provisioning).
 * Populated once by build_mqtt_identity() at the top of aws_iot_task, which
 * only runs when provisioning_is_active() (provisioning.h). MQTT_COMMAND_TOPIC
 * and friends below resolve to fields of this struct so every existing usage
 * site keeps working unchanged.
 */
typedef struct {
    char client_id[48];
    uint16_t client_id_len;
    char endpoint[128];
    const char *cert_pem;
    size_t cert_pem_len;
    const char *key_pem;
    size_t key_pem_len;
    /* Base is at most sourceTerminalId(40) + "/" + deviceType + "/" + deviceId(40);
     * sized generously since PROV_SOURCE_TERMINAL_ID_MAX/PROV_DEVICE_ID_MAX are
     * safety pads, not typical ULID lengths (provisioning.h). */
    char command_topic[160];
    uint16_t command_topic_len;
    char get_info_command_topic[160];
    uint16_t get_info_command_topic_len;
    char update_fw_command_topic[160];
    uint16_t update_fw_command_topic_len;
    char response_topic[160];
    uint16_t response_topic_len;
    char get_info_response_topic[160];
    uint16_t get_info_response_topic_len;
    char update_fw_response_topic[160];
    uint16_t update_fw_response_topic_len;
    /* <base>/pairing — device -> backend pairing events (security/pairing_events.h). */
    char pairing_topic[160];
    uint16_t pairing_topic_len;
} mqtt_identity_t;

extern mqtt_identity_t g_mqtt_identity;

/* Fills g_mqtt_identity from the provisioned per-device identity. Call once,
 * before the connect loop in aws_iot_task — NOT on every reconnect attempt (a
 * completed handshake applies itself via esp_restart(), see provisioning.c, so
 * the task never needs to notice a mid-flight identity change). */
void build_mqtt_identity(void);

#define MQTT_ACTIVE_ENDPOINT            g_mqtt_identity.endpoint
#define MQTT_ACTIVE_ENDPOINT_LENGTH     ( ( uint16_t ) strlen( g_mqtt_identity.endpoint ) )
#define MQTT_ACTIVE_CLIENT_ID           g_mqtt_identity.client_id
#define MQTT_ACTIVE_CLIENT_ID_LENGTH    g_mqtt_identity.client_id_len
#define AWS_IOT_MQTT_ALPN               "x-amzn-mqtt-ca"
#define AWS_IOT_MQTT_ALPN_LENGTH        ( ( uint16_t ) ( sizeof( AWS_IOT_MQTT_ALPN ) - 1 ) )
#define AWS_IOT_PASSWORD_ALPN           "mqtt"
#define AWS_IOT_PASSWORD_ALPN_LENGTH    ( ( uint16_t ) ( sizeof( AWS_IOT_PASSWORD_ALPN ) - 1 ) )
#define CONNECTION_RETRY_MAX_ATTEMPTS            ( 5U )
#define CONNECTION_RETRY_MAX_BACKOFF_DELAY_MS    ( 5000U )
#define CONNECTION_RETRY_BACKOFF_BASE_MS         ( 500U )
#define CONNACK_RECV_TIMEOUT_MS                  ( 1000U )

/*
 * Topics are runtime values (built from the provisioned
 * sourceTerminalId/deviceType/deviceId by build_mqtt_identity()), not
 * compile-time string literals. Every call site reads them as plain
 * char-pointer / uint16_t expressions, so the macro substitution is unchanged
 * at the use site.
 */
#define MQTT_COMMAND_TOPIC                  g_mqtt_identity.command_topic
#define MQTT_COMMAND_TOPIC_LENGTH           g_mqtt_identity.command_topic_len

#define MQTT_GET_INFO_COMMAND_TOPIC         g_mqtt_identity.get_info_command_topic
#define MQTT_GET_INFO_COMMAND_TOPIC_LENGTH  g_mqtt_identity.get_info_command_topic_len

#define MQTT_UPDATE_FW_COMMAND_TOPIC         g_mqtt_identity.update_fw_command_topic
#define MQTT_UPDATE_FW_COMMAND_TOPIC_LENGTH  g_mqtt_identity.update_fw_command_topic_len

#define MQTT_RESPONSE_TOPIC                  g_mqtt_identity.response_topic
#define MQTT_RESPONSE_TOPIC_LENGTH           g_mqtt_identity.response_topic_len

#define MQTT_GET_INFO_RESPONSE_TOPIC         g_mqtt_identity.get_info_response_topic
#define MQTT_GET_INFO_RESPONSE_TOPIC_LENGTH  g_mqtt_identity.get_info_response_topic_len

#define MQTT_UPDATE_FW_RESPONSE_TOPIC         g_mqtt_identity.update_fw_response_topic
#define MQTT_UPDATE_FW_RESPONSE_TOPIC_LENGTH  g_mqtt_identity.update_fw_response_topic_len

#define MQTT_PAIRING_TOPIC                    g_mqtt_identity.pairing_topic
#define MQTT_PAIRING_TOPIC_LENGTH             g_mqtt_identity.pairing_topic_len

#define MAX_OUTGOING_PUBLISHES              ( 5U )
#define MQTT_PACKET_ID_INVALID              ( ( uint16_t ) 0U )
#define MQTT_PROCESS_LOOP_TIMEOUT_MS        ( 5000U )
#define MQTT_KEEP_ALIVE_INTERVAL_SECONDS    ( 60U )
#define DELAY_BETWEEN_PUBLISHES_SECONDS     ( 1U )
#define MQTT_PUBLISH_COUNT_PER_LOOP         ( 5U )
#define MQTT_SUBPUB_LOOP_DELAY_SECONDS      ( 2U )
#define TRANSPORT_SEND_RECV_TIMEOUT_MS      ( 1500U )
#define METRICS_STRING                      "?SDK=" OS_NAME "&Version=" OS_VERSION "&Platform=" HARDWARE_PLATFORM_NAME "&MQTTLib=" MQTT_LIB
#define METRICS_STRING_LENGTH               ( ( uint16_t ) ( sizeof( METRICS_STRING ) - 1 ) )


#ifdef CLIENT_USERNAME

/**
 * @brief Append the username with the metrics string if #CLIENT_USERNAME is defined.
 *
 * This is to support both metrics reporting and username/password based client
 * authentication by AWS IoT.
 */
    #define CLIENT_USERNAME_WITH_METRICS    CLIENT_USERNAME METRICS_STRING
#endif
#define OUTGOING_PUBLISH_RECORD_LEN    ( 10U )
#define INCOMING_PUBLISH_RECORD_LEN    ( 10U )

extern uint16_t globalAckPacketIdentifier;
extern uint16_t globalSubscribePacketIdentifier;
extern uint16_t globalUnsubscribePacketIdentifier;
extern MQTTSubscribeInfo_t pGlobalSubscriptionList[ SUBSCRIBE_TOPICS_SIZE ];
extern uint8_t buffer[ NETWORK_BUFFER_SIZE ];
extern MQTTSubAckStatus_t globalSubAckStatus;
extern MQTTPubAckInfo_t pOutgoingPublishRecords[ OUTGOING_PUBLISH_RECORD_LEN ];
extern MQTTPubAckInfo_t pIncomingPublishRecords[ INCOMING_PUBLISH_RECORD_LEN ];
extern StaticSemaphore_t xTlsContextSemaphoreBuffer;

void aws_iot_task(void *pvParameters);
void command_buffer_handler();
int establishMqttSession( MQTTContext_t * pMqttContext,
                                 bool createCleanSession,
                                 bool * pSessionPresent );
int publishToTopic( MQTTContext_t * pMqttContext , char *msg_topic  , char *message );
