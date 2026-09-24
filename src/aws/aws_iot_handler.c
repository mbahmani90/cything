#include "aws_iot_handler.h"
#include "CyThingEsp32.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#include "device_config/device_config.h"
#include "aws/root_cert_auth.h"
#include "common/cy_log.h"
#include "common/response_fifo.h"
#include "aws/mqtt_response_fifo.h"
#include "security/pairing_events.h"
#include "aws/provisioning.h"
#include "ota_lib/http_ota_handler.h"
#include "tcp_server/tcp_client_list.h"
#include "tcp_server/tcp_command.h"
#include "udp_socket/udp_response_handler.h"
#include "wifi/station_mode.h"
#include "device_config/cy_config.h"

bool is_connected_to_server = false;

/* Remote drain state, used only from aws_iot_task (processLoopWithTimeout). */
static char       mqtt_staging[ RESPONSE_FIFO_SIZE + 1 ];
static TickType_t mqtt_last_send_tick = 0;
/* One pairing event per second at most; the payload buffer must outlive
 * the QoS1 publish like mqtt_staging does. */
static TickType_t pairing_last_send_tick = 0;
static char       pairing_event_buf[ PAIRING_EVENT_JSON_MAX ];

typedef struct PublishPackets
{
    uint16_t packetId;
    MQTTPublishInfo_t pubInfo;
} PublishPackets_t;

/* Populated at runtime by build_mqtt_identity() — the topics now depend on
 * which identity (legacy vs. provisioned) is active, so they can no longer
 * be static-initialized from compile-time string literals. */
const char *subscribeTopics[3];
uint16_t subscribeTopicLengths[3];

uint16_t globalAckPacketIdentifier = 0U;
uint16_t globalSubscribePacketIdentifier = 0U;
uint16_t globalUnsubscribePacketIdentifier = 0U;
PublishPackets_t outgoingPublishPackets[ MAX_OUTGOING_PUBLISHES ] = { 0 };
MQTTSubscribeInfo_t pGlobalSubscriptionList[ SUBSCRIBE_TOPICS_SIZE ];
uint8_t buffer[ NETWORK_BUFFER_SIZE ];
MQTTSubAckStatus_t globalSubAckStatus = MQTTSubAckFailure;
MQTTPubAckInfo_t pOutgoingPublishRecords[ OUTGOING_PUBLISH_RECORD_LEN ];
MQTTPubAckInfo_t pIncomingPublishRecords[ INCOMING_PUBLISH_RECORD_LEN ];
StaticSemaphore_t xTlsContextSemaphoreBuffer;

static int initializeMqtt( MQTTContext_t * pMqttContext,
                           NetworkContext_t * pNetworkContext );

void process_aws_iot_get_info_command( MQTTContext_t * pMqttContext, const char *command , int command_length){

    char *temp_response = (char *) calloc(320 , sizeof(char));

    if(!memcmp(command, cy_scan_command, cy_scan_command_len())){
        /* Remote get-info reply, 8-field per-device-cert layout:
         *   scanResponse,sourceTerminalId,deviceType,deviceId,localIp,ssidB64,hardwareVersion,firmwareVersion
         *
         * localIp + ssidB64 (base64 of the joined SSID) let a co-located app
         * bring up a direct LAN socket when the multicast scan can't cross APs
         * — see the CypressTerminalKmp repo, doc/local-ip-discovery-via-mqtt.md.
         * The SSID is base64'd because an 802.11 SSID may contain a comma;
         * localIp is left empty when the STA has no address, and the app then
         * just stays on the metered cloud path. hardwareVersion/firmwareVersion
         * move to the tail; an app built before this still parses the 6-field
         * form, and this device only ever emits the 8-field one.
         *
         * The app matches "<sourceTerminalId>/<deviceType>/<deviceId>" against
         * the stored deviceTopicId. aws_iot_task only runs once
         * provisioning_is_active() (wifi/station_mode.c), so the per-device identity
         * is always available here. */
        char source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
        char device_id[PROV_DEVICE_ID_MAX];
        char provision_state[16];
        provisioning_get_scan_fields(source_terminal_id, sizeof(source_terminal_id),
            device_id, sizeof(device_id), provision_state, sizeof(provision_state));

        /* Current STA address (device_ip is refreshed on every
         * IP_EVENT_STA_GOT_IP — wifi/station_mode.c). Empty while the STA is down. */
        char local_ip[16] = "";
        if (isConnectedToWifi) {
            get_device_wifi_info(local_ip);
        }

        /* base64 of the joined SSID (ssid_arg, wifi/station_mode.c), capped at the
         * 802.11 max of 32 bytes. Empty on encode failure or no SSID. */
        char ssid_b64[48] = "";
        size_t ssid_len = strlen(ssid_arg);
        if (ssid_len > 32) {
            ssid_len = 32;
        }
        if (ssid_len > 0) {
            size_t olen = 0;
            if (mbedtls_base64_encode((unsigned char *) ssid_b64, sizeof(ssid_b64), &olen,
                    (const unsigned char *) ssid_arg, ssid_len) == 0 && olen < sizeof(ssid_b64)) {
                ssid_b64[olen] = '\0';
            } else {
                ssid_b64[0] = '\0';
            }
        }

        snprintf(temp_response, 320, "%s,%s,%s,%s,%s,%s,%s,%s",
            cy_scan_response, source_terminal_id, cy_device_type, device_id,
            local_ip, ssid_b64, cy_hardware_version, cy_firmware_version);
        publishToTopic(pMqttContext , MQTT_GET_INFO_RESPONSE_TOPIC , temp_response);
        free(temp_response);
        return;
    }


}

void process_aws_iot_update_fw_response( MQTTContext_t * pMqttContext, const char *command , int command_length){

    char *temp_response = (char *) calloc(256 , sizeof(char)); 

    if(!memcmp(command , updateFirmwareSteps[UPDATE_FW_START].command , 15)){
        sprintf(temp_response, "%s", updateFirmwareSteps[UPDATE_FW_START].response);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        publishToTopic(pMqttContext , MQTT_UPDATE_FW_RESPONSE_TOPIC , temp_response);
    }
    else if(memcmp(command , updateFirmwareSteps[UPDATE_FW_CREDENTIAL].command , 14) == 0){
        sprintf(temp_response , "%s" , updateFirmwareSteps[UPDATE_FW_CREDENTIAL].response);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        publishToTopic(pMqttContext , MQTT_UPDATE_FW_RESPONSE_TOPIC , temp_response);
    }
    else if(memcmp(command , updateFirmwareSteps[UPDATE_FW_UPDATING].command , 12) == 0){
        memcpy(firmwareUrl , command + 12 , command_length - 12);
        firmwareUrl[command_length - 12] = 0;
        // xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
        sprintf(temp_response , "%s2\n" , updateFirmwareSteps[UPDATE_FW_UPDATING].response);
        publishToTopic(pMqttContext , MQTT_UPDATE_FW_RESPONSE_TOPIC , temp_response);
    }

    free(temp_response);

}

/* Set while process_aws_iot_command runs the app hook, so
 * mqtt_publish_response() can reach the live session. */
static MQTTContext_t *s_command_mqtt_context = NULL;

void process_aws_iot_command( MQTTContext_t * pMqttContext, const char *command , int command_length){

    CY_LOGI(AWS_IOT_DB, "%.*s   %d\r\n" , command_length , command , command_length);

    /* Application commands live in app_main.c (app_mqtt_command_handle).
     * mqtt_publish_response() needs the context of the session that
     * delivered this command; it is only valid for the duration of the call. */
    s_command_mqtt_context = pMqttContext;
    /* No canned reply: what a command answers is the application's business.
     * An unclaimed command is logged and left alone rather than being given a
     * response the sketch never chose. */
    if(!app_mqtt_command_handle(command, command_length)){
        CY_LOGW(AWS_IOT_DB, "unrecognised MQTT command ignored: %.*s",
                command_length, command);
    }
    s_command_mqtt_context = NULL;

}

void mqtt_publish_response(const char *text){

    if(s_command_mqtt_context == NULL){
        CY_LOGW(AWS_IOT_DB, "mqtt_publish_response called outside app_mqtt_command_handle; dropped");
        return;
    }
    char line[256];
    int id = update_response_id();
    snprintf(line, sizeof(line), "%d:%s", id , text);
    publishToTopic(s_command_mqtt_context , MQTT_RESPONSE_TOPIC , line);

}

/* Fallback for app_mqtt_command_handle(); overridden by the definition in
 * app_main.c. */
__attribute__((weak)) bool app_mqtt_command_handle(const char *command , int command_length){
    (void)command; (void)command_length;
    return false;
}


static uint32_t generateRandomNumber()
{
    return( rand() );
}

static void cleanupESPSecureMgrCerts( NetworkContext_t * pNetworkContext )
{
    /* The cert/key point into g_mqtt_identity (provisioning.c), which owns
     * them — nothing to free here. */
    (void)pNetworkContext;
}

static int connectToServerWithBackoffRetries( NetworkContext_t * pNetworkContext,
                                              MQTTContext_t * pMqttContext,
                                              bool * pClientSessionPresent,
                                              bool * pBrokerSessionPresent )
{

    int returnStatus = EXIT_SUCCESS;
    BackoffAlgorithmStatus_t backoffAlgStatus = BackoffAlgorithmSuccess;
    TlsTransportStatus_t tlsStatus = TLS_TRANSPORT_SUCCESS;
    BackoffAlgorithmContext_t reconnectParams;
    bool createCleanSession;

    pNetworkContext->pcHostname = MQTT_ACTIVE_ENDPOINT;
    pNetworkContext->xPort = AWS_MQTT_PORT;
    pNetworkContext->pxTls = NULL;
    pNetworkContext->xTlsContextSemaphore = xSemaphoreCreateMutexStatic(&xTlsContextSemaphoreBuffer);

    pNetworkContext->disableSni = 0;
    uint16_t nextRetryBackOff;

    /* Initialize credentials for establishing TLS session. */
    pNetworkContext->pcServerRootCA     = root_cert_auth_pem;
    pNetworkContext->pcServerRootCASize = root_cert_auth_pem_len;

    /* If #CLIENT_USERNAME is defined, username/password is used for authenticating
     * the client. */
    #ifndef CLIENT_USERNAME
        pNetworkContext->pcClientCert = g_mqtt_identity.cert_pem;
        pNetworkContext->pcClientCertSize = g_mqtt_identity.cert_pem_len;
        pNetworkContext->pcClientKey = g_mqtt_identity.key_pem;
        pNetworkContext->pcClientKeySize = g_mqtt_identity.key_pem_len;
    #endif
    /* AWS IoT requires devices to send the Server Name Indication (SNI)
     * extension to the Transport Layer Security (TLS) protocol and provide
     * the complete endpoint address in the host_name field. Details about
     * SNI for AWS IoT can be found in the link below.
     * https://docs.aws.amazon.com/iot/latest/developerguide/transport-security.html */

    if( AWS_MQTT_PORT == 443 )
    {
        /* Pass the ALPN protocol name depending on the port being used.
         * Please see more details about the ALPN protocol for the AWS IoT MQTT
         * endpoint in the link below.
         * https://aws.amazon.com/blogs/iot/mqtt-with-tls-client-authentication-on-port-443-why-it-is-useful-and-how-it-works/
         *
         * For username and password based authentication in AWS IoT,
         * #AWS_IOT_PASSWORD_ALPN is used. More details can be found in the
         * link below.
         * https://docs.aws.amazon.com/iot/latest/developerguide/custom-authentication.html
         */

        static const char * pcAlpnProtocols[] = { NULL, NULL };

        #ifdef CLIENT_USERNAME
            pcAlpnProtocols[0] = AWS_IOT_PASSWORD_ALPN;
        #else
            pcAlpnProtocols[0] = AWS_IOT_MQTT_ALPN;
        #endif

        pNetworkContext->pAlpnProtos = pcAlpnProtocols;
    } else {
        pNetworkContext->pAlpnProtos = NULL;
    }

    /* Initialize reconnect attempts and interval */
    BackoffAlgorithm_InitializeParams( &reconnectParams,
                                       CONNECTION_RETRY_BACKOFF_BASE_MS,
                                       CONNECTION_RETRY_MAX_BACKOFF_DELAY_MS,
                                       CONNECTION_RETRY_MAX_ATTEMPTS );

    /* Attempt to connect to MQTT broker. If connection fails, retry after
     * a timeout. Timeout value will exponentially increase until maximum
     * attempts are reached.
     */
    do
    {
        /* Establish a TLS session with the MQTT broker. This example connects
         * to the MQTT broker as specified in AWS_IOT_ENDPOINT and AWS_MQTT_PORT
         * at the demo config header. */

        CY_LOGI(AWS_IOT_DB, "Establishing a TLS session to %.*s:%d.",
                   MQTT_ACTIVE_ENDPOINT_LENGTH,
                   MQTT_ACTIVE_ENDPOINT,
                   AWS_MQTT_PORT);

        // tlsStatus = xTlsDisconnect ( pNetworkContext );
        tlsStatus = xTlsConnect ( pNetworkContext );

        if( tlsStatus == TLS_TRANSPORT_SUCCESS )
        {
            /* A clean MQTT session needs to be created, if there is no session saved
             * in this MQTT client. */
            createCleanSession = ( *pClientSessionPresent == true ) ? false : true;
            /* Sends an MQTT Connect packet using the established TLS session,
             * then waits for connection acknowledgment (CONNACK) packet. */
            returnStatus = establishMqttSession( pMqttContext, createCleanSession, pBrokerSessionPresent );

            if( returnStatus == EXIT_FAILURE )
            {
                /* End TLS session, then close TCP connection. */
                cleanupESPSecureMgrCerts( pNetworkContext );
                ( void ) xTlsDisconnect( pNetworkContext );
            }
        }else if (tlsStatus == TLS_TRANSPORT_CONNECT_FAILURE){
            cleanupESPSecureMgrCerts( pNetworkContext );
            ( void ) xTlsDisconnect( pNetworkContext );
            returnStatus = EXIT_FAILURE;
        }

        if( returnStatus == EXIT_FAILURE )
        {
            /* Generate a random number and get back-off value (in milliseconds) for the next connection retry. */
            backoffAlgStatus = BackoffAlgorithm_GetNextBackoff( &reconnectParams, generateRandomNumber(), &nextRetryBackOff );

            if( backoffAlgStatus == BackoffAlgorithmRetriesExhausted )
            {
             
                CY_LOGE(AWS_IOT_DB, "Connection to the broker failed, all attempts exhausted." );
                returnStatus = EXIT_FAILURE;
            }
            else if( backoffAlgStatus == BackoffAlgorithmSuccess )
            {
                CY_LOGW(AWS_IOT_DB, "Connection to the broker failed. Retrying connection "
                           "after %hu ms backoff.",
                           ( unsigned short ) nextRetryBackOff );
                Clock_SleepMs( nextRetryBackOff );
            }
        }
    } while( ( returnStatus == EXIT_FAILURE ) && ( backoffAlgStatus == BackoffAlgorithmSuccess ) );

    return returnStatus;
}

/*-----------------------------------------------------------*/

static int getNextFreeIndexForOutgoingPublishes( uint8_t * pIndex )
{
    int returnStatus = EXIT_FAILURE;
    uint8_t index = 0;

    assert( outgoingPublishPackets != NULL );
    assert( pIndex != NULL );

    for( index = 0; index < MAX_OUTGOING_PUBLISHES; index++ )
    {
        /* A free index is marked by invalid packet id.
         * Check if the the index has a free slot. */
        if( outgoingPublishPackets[ index ].packetId == MQTT_PACKET_ID_INVALID )
        {
            returnStatus = EXIT_SUCCESS;
            break;
        }
    }

    /* Copy the available index into the output param. */
    *pIndex = index;

    return returnStatus;
}
/*-----------------------------------------------------------*/

static void cleanupOutgoingPublishAt( uint8_t index )
{

    assert( outgoingPublishPackets != NULL );
    assert( index < MAX_OUTGOING_PUBLISHES );

    /* Clear the outgoing publish packet. */
    ( void ) memset( &( outgoingPublishPackets[ index ] ),
                     0x00,
                     sizeof( outgoingPublishPackets[ index ] ) );
}

/*-----------------------------------------------------------*/

static void cleanupOutgoingPublishes( void )
{

    assert( outgoingPublishPackets != NULL );

    /* Clean up all the outgoing publish packets. */
    ( void ) memset( outgoingPublishPackets, 0x00, sizeof( outgoingPublishPackets ) );
}

/*-----------------------------------------------------------*/

static void cleanupOutgoingPublishWithPacketID( uint16_t packetId )
{

    uint8_t index = 0;

    assert( outgoingPublishPackets != NULL );
    assert( packetId != MQTT_PACKET_ID_INVALID );

    /* Clean up all the saved outgoing publishes. */
    for( ; index < MAX_OUTGOING_PUBLISHES; index++ )
    {
        if( outgoingPublishPackets[ index ].packetId == packetId )
        {
            cleanupOutgoingPublishAt( index );
            CY_LOGI(AWS_IOT_DB, "Cleaned up outgoing publish packet with packet id %u.\n\n",
                       packetId );
            break;
        }
    }
}

/*-----------------------------------------------------------*/

static int handlePublishResend( MQTTContext_t * pMqttContext )
{

    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus = MQTTSuccess;
    uint8_t index = 0U;
    MQTTStateCursor_t cursor = MQTT_STATE_CURSOR_INITIALIZER;
    uint16_t packetIdToResend = MQTT_PACKET_ID_INVALID;
    bool foundPacketId = false;

    assert( pMqttContext != NULL );
    assert( outgoingPublishPackets != NULL );

    /* MQTT_PublishToResend() provides a packet ID of the next PUBLISH packet
     * that should be resent. In accordance with the MQTT v3.1.1 spec,
     * MQTT_PublishToResend() preserves the ordering of when the original
     * PUBLISH packets were sent. The outgoingPublishPackets array is searched
     * through for the associated packet ID. If the application requires
     * increased efficiency in the look up of the packet ID, then a hashmap of
     * packetId key and PublishPacket_t values may be used instead. */
    packetIdToResend = MQTT_PublishToResend( pMqttContext, &cursor );

    while( packetIdToResend != MQTT_PACKET_ID_INVALID )
    {
        foundPacketId = false;

        for( index = 0U; index < MAX_OUTGOING_PUBLISHES; index++ )
        {
            if( outgoingPublishPackets[ index ].packetId == packetIdToResend )
            {
                foundPacketId = true;
                outgoingPublishPackets[ index ].pubInfo.dup = true;
                CY_LOGI(AWS_IOT_DB, "Sending duplicate PUBLISH with packet id %u.",
                           outgoingPublishPackets[ index ].packetId );
                mqttStatus = MQTT_Publish( pMqttContext,
                                           &outgoingPublishPackets[ index ].pubInfo,
                                           outgoingPublishPackets[ index ].packetId );

                if( mqttStatus != MQTTSuccess )
                {
                    CY_LOGE(AWS_IOT_DB, "Sending duplicate PUBLISH for packet id %u "
                                " failed with status %s.",
                                outgoingPublishPackets[ index ].packetId,
                                MQTT_Status_strerror( mqttStatus ) );
                    returnStatus = EXIT_FAILURE;
                    break;
                }
                else
                {
                    CY_LOGI(AWS_IOT_DB, "Sent duplicate PUBLISH successfully for packet id %u.\n\n",
                               outgoingPublishPackets[ index ].packetId );
                }
            }
        }

        if( foundPacketId == false )
        {
            CY_LOGE(AWS_IOT_DB, "Packet id %u requires resend, but was not found in "
                        "outgoingPublishPackets.",
                        packetIdToResend );
            returnStatus = EXIT_FAILURE;
            break;
        }
        else
        {
            /* Get the next packetID to be resent. */
            packetIdToResend = MQTT_PublishToResend( pMqttContext, &cursor );
        }
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

static void handleIncomingPublish( MQTTContext_t * pMqttContext, 
                                    MQTTPublishInfo_t * pPublishInfo,
                                   uint16_t packetIdentifier )
{

    assert( pPublishInfo != NULL );

    /* Process incoming Publish. */
    // CY_LOGI(AWS_IOT_DB,  "Incoming QOS : %d.", pPublishInfo->qos);

    bool isTopicMatched = true;

    /* Verify the received publish is for the topic we have subscribed to. */
    if( ( pPublishInfo->topicNameLength == MQTT_COMMAND_TOPIC_LENGTH ) &&
        ( 0 == strncmp( MQTT_COMMAND_TOPIC,
                        pPublishInfo->pTopicName,
                        pPublishInfo->topicNameLength ) ) )
    {
        process_aws_iot_command(pMqttContext , 
            ( const char * ) pPublishInfo->pPayload , 
            ( int ) pPublishInfo->payloadLength);
    }
    else if( ( pPublishInfo->topicNameLength == MQTT_GET_INFO_COMMAND_TOPIC_LENGTH ) &&
        ( 0 == strncmp( MQTT_GET_INFO_COMMAND_TOPIC,
                        pPublishInfo->pTopicName,
                        pPublishInfo->topicNameLength ) ) )
    {
        process_aws_iot_get_info_command(pMqttContext , 
            ( const char * ) pPublishInfo->pPayload , 
            ( int ) pPublishInfo->payloadLength);
    }
    else if( ( pPublishInfo->topicNameLength == MQTT_UPDATE_FW_COMMAND_TOPIC_LENGTH ) &&
        ( 0 == strncmp( MQTT_UPDATE_FW_COMMAND_TOPIC,
                        pPublishInfo->pTopicName,
                        pPublishInfo->topicNameLength ) ) )
    {
        process_aws_iot_update_fw_response(pMqttContext , 
            ( const char * ) pPublishInfo->pPayload , 
            ( int ) pPublishInfo->payloadLength);
    }
    else
    {
        isTopicMatched = false;
        // CY_LOGI(AWS_IOT_DB,  "Incoming Publish Topic Name: %.*s does not match subscribed topic.",
        //            pPublishInfo->topicNameLength,
        //            pPublishInfo->pTopicName );
    }

    if(isTopicMatched){
        // CY_LOGI(AWS_IOT_DB,  "Incoming Publish Topic Name: %.*s matches subscribed topic.\n"
        //            "Incoming Publish message Packet Id is %u.\n"
        //            "Incoming Publish Message : %.*s.\n\n",
        //            pPublishInfo->topicNameLength,
        //            pPublishInfo->pTopicName,
        //            packetIdentifier,
        //            ( int ) pPublishInfo->payloadLength,
        //            ( const char * ) pPublishInfo->pPayload );
    }
}

/*-----------------------------------------------------------*/

static void updateSubAckStatus( MQTTPacketInfo_t * pPacketInfo )
{

    uint8_t * pPayload = NULL;
    size_t pSize = 0;

    MQTTStatus_t mqttStatus = MQTT_GetSubAckStatusCodes( pPacketInfo, &pPayload, &pSize );

    /* MQTT_GetSubAckStatusCodes always returns success if called with packet info
     * from the event callback and non-NULL parameters. */
    assert( mqttStatus == MQTTSuccess );

    /* Suppress unused variable warning when asserts are disabled in build. */
    ( void ) mqttStatus;

    /* Demo only subscribes to one topic, so only one status code is returned. */
    globalSubAckStatus = ( MQTTSubAckStatus_t ) pPayload[ 0 ];
}


static void eventCallback( MQTTContext_t * pMqttContext,
                           MQTTPacketInfo_t * pPacketInfo,
                           MQTTDeserializedInfo_t * pDeserializedInfo )
{
    uint16_t packetIdentifier;

    assert( pMqttContext != NULL );
    assert( pPacketInfo != NULL );
    assert( pDeserializedInfo != NULL );

    /* Suppress unused parameter warning when asserts are disabled in build. */
    ( void ) pMqttContext;

    packetIdentifier = pDeserializedInfo->packetIdentifier;

    /* Handle incoming publish. The lower 4 bits of the publish packet
     * type is used for the dup, QoS, and retain flags. Hence masking
     * out the lower bits to check if the packet is publish. */
    if( ( pPacketInfo->type & 0xF0U ) == MQTT_PACKET_TYPE_PUBLISH )
    {
        assert( pDeserializedInfo->pPublishInfo != NULL );
        /* Handle incoming publish. */
        handleIncomingPublish(pMqttContext, pDeserializedInfo->pPublishInfo, packetIdentifier );
    }
    else
    {
        /* Handle other packets. */
        switch( pPacketInfo->type )
        {
            case MQTT_PACKET_TYPE_SUBACK:

                /* A SUBACK from the broker, containing the server response to our subscription request, has been received.
                 * It contains the status code indicating server approval/rejection for the subscription to the single topic
                 * requested. The SUBACK will be parsed to obtain the status code, and this status code will be stored in global
                 * variable globalSubAckStatus. */
                updateSubAckStatus( pPacketInfo );

                /* Check status of the subscription request. If globalSubAckStatus does not indicate
                 * server refusal of the request (MQTTSubAckFailure), it contains the QoS level granted
                 * by the server, indicating a successful subscription attempt. */
                if( globalSubAckStatus != MQTTSubAckFailure )
                {
                    CY_LOGI(AWS_IOT_DB, "Subscribed to the topic %.*s. with maximum QoS %u.\n\n",
                               MQTT_COMMAND_TOPIC_LENGTH,
                               MQTT_COMMAND_TOPIC,
                               globalSubAckStatus );
                }

                /* Make sure ACK packet identifier matches with Request packet identifier. */
                assert( globalSubscribePacketIdentifier == packetIdentifier );

                /* Update the global ACK packet identifier. */
                globalAckPacketIdentifier = packetIdentifier;
                break;

            case MQTT_PACKET_TYPE_UNSUBACK:
                CY_LOGI(AWS_IOT_DB, "Unsubscribed from the topic %.*s.\n\n",
                           MQTT_COMMAND_TOPIC_LENGTH,
                           MQTT_COMMAND_TOPIC );
                /* Make sure ACK packet identifier matches with Request packet identifier. */
                assert( globalUnsubscribePacketIdentifier == packetIdentifier );

                /* Update the global ACK packet identifier. */
                globalAckPacketIdentifier = packetIdentifier;
                break;

            case MQTT_PACKET_TYPE_PINGRESP:
                /* Nothing to be done from application as library handles
                 * PINGRESP. */
                CY_LOGW(AWS_IOT_DB, "PINGRESP should not be handled by the application "
                           "callback when using MQTT_ProcessLoop.\n\n");
                break;

            case MQTT_PACKET_TYPE_PUBACK:
                CY_LOGI(AWS_IOT_DB, "PUBACK received for packet id %u.\n\n",
                           packetIdentifier );
                /* Cleanup publish packet when a PUBACK is received. */
                cleanupOutgoingPublishWithPacketID( packetIdentifier );

                /* Update the global ACK packet identifier. */
                globalAckPacketIdentifier = packetIdentifier;
                break;

            /* Any other packet type is invalid. */
            default:

                CY_LOGE(AWS_IOT_DB, "Unknown packet type received:(%02x).\n\n",
                            pPacketInfo->type );

        }
    }
}

/*-----------------------------------------------------------*/

int establishMqttSession( MQTTContext_t * pMqttContext,
                                 bool createCleanSession,
                                 bool * pSessionPresent )
{
    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus;
    MQTTConnectInfo_t connectInfo = { 0 };

    assert( pMqttContext != NULL );
    assert( pSessionPresent != NULL );

    /* Establish MQTT session by sending a CONNECT packet. */

    /* If #createCleanSession is true, start with a clean session
     * i.e. direct the MQTT broker to discard any previous session data.
     * If #createCleanSession is false, directs the broker to attempt to
     * reestablish a session which was already present. */
    connectInfo.cleanSession = createCleanSession;

    /* The client identifier is used to uniquely identify this MQTT client to
     * the MQTT broker. In a production device the identifier can be something
     * unique, such as a device serial number. */
    connectInfo.pClientIdentifier = MQTT_ACTIVE_CLIENT_ID;
    connectInfo.clientIdentifierLength = MQTT_ACTIVE_CLIENT_ID_LENGTH;

    /* The maximum time interval in seconds which is allowed to elapse
     * between two Control Packets.
     * It is the responsibility of the Client to ensure that the interval between
     * Control Packets being sent does not exceed the this Keep Alive value. In the
     * absence of sending any other Control Packets, the Client MUST send a
     * PINGREQ Packet. */
    connectInfo.keepAliveSeconds = MQTT_KEEP_ALIVE_INTERVAL_SECONDS;

    /* Use the username and password for authentication, if they are defined.
     * Refer to the AWS IoT documentation below for details regarding client
     * authentication with a username and password.
     * https://docs.aws.amazon.com/iot/latest/developerguide/custom-authentication.html
     * An authorizer setup needs to be done, as mentioned in the above link, to use
     * username/password based client authentication.
     *
     * The username field is populated with voluntary metrics to AWS IoT.
     * The metrics collected by AWS IoT are the operating system, the operating
     * system's version, the hardware platform, and the MQTT Client library
     * information. These metrics help AWS IoT improve security and provide
     * better technical support.
     *
     * If client authentication is based on username/password in AWS IoT,
     * the metrics string is appended to the username to support both client
     * authentication and metrics collection. */
    #ifdef CLIENT_USERNAME
        connectInfo.pUserName = CLIENT_USERNAME_WITH_METRICS;
        connectInfo.userNameLength = strlen( CLIENT_USERNAME_WITH_METRICS );
        connectInfo.pPassword = CLIENT_PASSWORD;
        connectInfo.passwordLength = strlen( CLIENT_PASSWORD );
    #else
        connectInfo.pUserName = METRICS_STRING;
        connectInfo.userNameLength = METRICS_STRING_LENGTH;
        /* Password for authentication is not used. */
        connectInfo.pPassword = NULL;
        connectInfo.passwordLength = 0U;
    #endif /* ifdef CLIENT_USERNAME */

    /* Send MQTT CONNECT packet to broker. */
    mqttStatus = MQTT_Connect( pMqttContext, &connectInfo, NULL, CONNACK_RECV_TIMEOUT_MS, pSessionPresent );

    if( mqttStatus != MQTTSuccess )
    {
        returnStatus = EXIT_FAILURE;
        CY_LOGE(AWS_IOT_DB, "Connection with MQTT broker failed with status %s.",
                    MQTT_Status_strerror(mqttStatus));
    }
    else
    {
        CY_LOGI(AWS_IOT_DB, "MQTT connection successfully established with broker.\n\n");
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

static int disconnectMqttSession( MQTTContext_t * pMqttContext )
{
    MQTTStatus_t mqttStatus = MQTTSuccess;
    int returnStatus = EXIT_SUCCESS;

    assert( pMqttContext != NULL );

    /* Send DISCONNECT. */
    mqttStatus = MQTT_Disconnect( pMqttContext );

    if( mqttStatus != MQTTSuccess )
    {
        CY_LOGE(AWS_IOT_DB, "Sending MQTT DISCONNECT failed with status=%s.",
                    MQTT_Status_strerror(mqttStatus));
        returnStatus = EXIT_FAILURE;
    }

    return returnStatus;
}



/*-----------------------------------------------------------*/

static int subscribeToTopics(MQTTContext_t *pMqttContext)
{
    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus;

    assert(pMqttContext != NULL);

    /* Clear the subscription list */
    (void) memset((void *)pGlobalSubscriptionList, 0x00, sizeof(pGlobalSubscriptionList));

    /* Fill subscription list with all topics */
    for (int i = 0; i < SUBSCRIBE_TOPICS_SIZE; i++)
    {
        pGlobalSubscriptionList[i].qos = MQTTQoS1;
        pGlobalSubscriptionList[i].pTopicFilter = (char *)subscribeTopics[i];
        pGlobalSubscriptionList[i].topicFilterLength = subscribeTopicLengths[i];
    }

    /* Generate packet identifier */
    globalSubscribePacketIdentifier = MQTT_GetPacketId(pMqttContext);

    /* Send SUBSCRIBE packet */
    mqttStatus = MQTT_Subscribe(pMqttContext,
                                pGlobalSubscriptionList,
                                SUBSCRIBE_TOPICS_SIZE,
                                globalSubscribePacketIdentifier);

    if (mqttStatus != MQTTSuccess)
    {
        CY_LOGE(AWS_IOT_DB, "Failed to send SUBSCRIBE packet to broker with error = %s.",
                  MQTT_Status_strerror(mqttStatus));
        returnStatus = EXIT_FAILURE;
    }
    else
    {
        CY_LOGI(AWS_IOT_DB, "SUBSCRIBE sent for %d topics to broker.\n\n", SUBSCRIBE_TOPICS_SIZE);
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/

static int unsubscribeFromTopics(MQTTContext_t *pMqttContext)
{
    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus;

    assert(pMqttContext != NULL);

    /* Clear the subscription list */
    (void) memset((void *)pGlobalSubscriptionList, 0x00, sizeof(pGlobalSubscriptionList));


    for (int i = 0; i < SUBSCRIBE_TOPICS_SIZE; i++)
    {
        pGlobalSubscriptionList[i].qos = MQTTQoS1; // QOS is not used for unsubscribe but kept for structure
        pGlobalSubscriptionList[i].pTopicFilter = (char *)subscribeTopics[i];
        pGlobalSubscriptionList[i].topicFilterLength = subscribeTopicLengths[i];
    }

    /* Generate packet identifier */
    globalUnsubscribePacketIdentifier = MQTT_GetPacketId(pMqttContext);

    /* Send UNSUBSCRIBE packet */
    mqttStatus = MQTT_Unsubscribe(pMqttContext,
                                  pGlobalSubscriptionList,
                                  SUBSCRIBE_TOPICS_SIZE,
                                  globalUnsubscribePacketIdentifier);

    if (mqttStatus != MQTTSuccess)
    {
        CY_LOGE(AWS_IOT_DB, "Failed to send UNSUBSCRIBE packet to broker with error = %s.",
                  MQTT_Status_strerror(mqttStatus));
        returnStatus = EXIT_FAILURE;
    }
    else
    {
        CY_LOGI(AWS_IOT_DB, "UNSUBSCRIBE sent for %d topics to broker.\n\n", SUBSCRIBE_TOPICS_SIZE);
    }

    return returnStatus;
}



int publishToTopic( MQTTContext_t * pMqttContext , char *msg_topic , char *message )
{
    
    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus = MQTTSuccess;
    uint8_t publishIndex = MAX_OUTGOING_PUBLISHES;

    assert( pMqttContext != NULL );


    /* Get the next free index for the outgoing publish. All QoS1 outgoing
     * publishes are stored until a PUBACK is received. These messages are
     * stored for supporting a resend if a network connection is broken before
     * receiving a PUBACK. */
    returnStatus = getNextFreeIndexForOutgoingPublishes( &publishIndex );

    if( returnStatus == EXIT_FAILURE )
    {
        CY_LOGE(AWS_IOT_DB, "Unable to find a free spot for outgoing PUBLISH message.\n\n");
    }
    else
    {
        /* This example publishes to only one topic and uses QOS1. */
        outgoingPublishPackets[ publishIndex ].pubInfo.qos = MQTTQoS1;
        outgoingPublishPackets[ publishIndex ].pubInfo.pTopicName = msg_topic;
        outgoingPublishPackets[ publishIndex ].pubInfo.topicNameLength = strlen(msg_topic);
        outgoingPublishPackets[ publishIndex ].pubInfo.pPayload = message;
        outgoingPublishPackets[ publishIndex ].pubInfo.payloadLength = strlen(message);
        outgoingPublishPackets[ publishIndex ].pubInfo.retain = false;
        outgoingPublishPackets[ publishIndex ].pubInfo.dup = false;

        /* Get a new packet id. */
        outgoingPublishPackets[ publishIndex ].packetId = MQTT_GetPacketId( pMqttContext );

        /* Send PUBLISH packet. */
        mqttStatus = MQTT_Publish( pMqttContext,
                                &outgoingPublishPackets[ publishIndex ].pubInfo,
                                outgoingPublishPackets[ publishIndex ].packetId ); //

        if( mqttStatus != MQTTSuccess )
        {
            CY_LOGE(AWS_IOT_DB, "Failed to send PUBLISH packet to broker with error = %s.",
                        MQTT_Status_strerror( mqttStatus ) );
            cleanupOutgoingPublishAt( publishIndex );
            // reconnect_to_server();
            returnStatus = EXIT_FAILURE;
        }
        else
        {
            // cleanupOutgoingPublishAt( publishIndex );
 
            CY_LOGI(AWS_IOT_DB, "PUBLISH sent for topic %.*s to broker with packet ID %u.\n\n",
                    MQTT_RESPONSE_TOPIC_LENGTH,
                    MQTT_RESPONSE_TOPIC,
                    outgoingPublishPackets[ publishIndex ].packetId);

        }



    }
    return returnStatus;
}

static int initializeMqtt( MQTTContext_t * pMqttContext,
                           NetworkContext_t * pNetworkContext )
{
    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus;
    MQTTFixedBuffer_t networkBuffer;
    TransportInterface_t transport = { 0 };

    assert( pMqttContext != NULL );
    assert( pNetworkContext != NULL );

    /* Fill in TransportInterface send and receive function pointers.
     * For this demo, TCP sockets are used to send and receive data
     * from network. Network context is SSL context for OpenSSL.*/
    transport.pNetworkContext = pNetworkContext;
    transport.send = espTlsTransportSend;
    transport.recv = espTlsTransportRecv;
    transport.writev = NULL;

    /* Fill the values for network buffer. */
    networkBuffer.pBuffer = buffer;
    networkBuffer.size = NETWORK_BUFFER_SIZE;
    /* Initialize MQTT library. */
    mqttStatus = MQTT_Init( pMqttContext,
                            &transport,
                            Clock_GetTimeMs,
                            eventCallback,
                            &networkBuffer );
    if( mqttStatus != MQTTSuccess )
    {
        returnStatus = EXIT_FAILURE;
        CY_LOGE(AWS_IOT_DB, "MQTT_Init failed: Status = %s.", MQTT_Status_strerror( mqttStatus ));
    }
    else
    {
        mqttStatus = MQTT_InitStatefulQoS( pMqttContext,
                                           pOutgoingPublishRecords,
                                           OUTGOING_PUBLISH_RECORD_LEN,
                                           pIncomingPublishRecords,
                                           INCOMING_PUBLISH_RECORD_LEN );
        if( mqttStatus != MQTTSuccess )
        {
            returnStatus = EXIT_FAILURE;

            CY_LOGE(AWS_IOT_DB, "MQTT_InitStatefulQoS failed: Status = %s.", MQTT_Status_strerror( mqttStatus ));

        }
    }

    return returnStatus;
}

static int waitForPacketAck( MQTTContext_t * pMqttContext,
                             uint16_t usPacketIdentifier,
                             uint32_t ulTimeout )
{
    // uint32_t ulMqttProcessLoopEntryTime;
    uint32_t ulMqttProcessLoopTimeoutTime;
    uint32_t ulCurrentTime;

    MQTTStatus_t eMqttStatus = MQTTSuccess;
    int returnStatus = EXIT_FAILURE;

    /* Reset the ACK packet identifier being received. */
    globalAckPacketIdentifier = 0U;

    ulCurrentTime = pMqttContext->getTime();
    // ulMqttProcessLoopEntryTime = ulCurrentTime;
    ulMqttProcessLoopTimeoutTime = ulCurrentTime + ulTimeout;

    /* Call MQTT_ProcessLoop multiple times until the expected packet ACK
     * is received, a timeout happens, or MQTT_ProcessLoop fails. */
    while( ( globalAckPacketIdentifier != usPacketIdentifier ) &&
           ( ulCurrentTime < ulMqttProcessLoopTimeoutTime ) &&
           ( eMqttStatus == MQTTSuccess || eMqttStatus == MQTTNeedMoreBytes ) )
    {
        /* Event callback will set #globalAckPacketIdentifier when receiving
         * appropriate packet. */
        eMqttStatus = MQTT_ProcessLoop( pMqttContext );
        ulCurrentTime = pMqttContext->getTime();
    }

    if( ( ( eMqttStatus != MQTTSuccess ) && ( eMqttStatus != MQTTNeedMoreBytes ) ) ||
        ( globalAckPacketIdentifier != usPacketIdentifier ) )
    {

        // CY_LOGE(AWS_IOT_DB, 
        //     "MQTT_ProcessLoop failed to receive ACK packet: Expected ACK Packet ID=%02\"PRIx16\", LoopDuration=%\"PRIu32\", Status=%s",
        //     usPacketIdentifier,
        //     ( ulCurrentTime - ulMqttProcessLoopTimeoutTime ),
        //     MQTT_Status_strerror( eMqttStatus )
        // );

    }
    else
    {
        returnStatus = EXIT_SUCCESS;
    }

    return returnStatus;
}

/*-----------------------------------------------------------*/


static MQTTStatus_t processLoopWithTimeout( MQTTContext_t * pMqttContext,
                                            uint32_t ulTimeoutMs )
{
    uint32_t ulMqttProcessLoopTimeoutTime;
    uint32_t ulCurrentTime;

    MQTTStatus_t eMqttStatus = MQTTSuccess;

    ulCurrentTime = pMqttContext->getTime();
    ulMqttProcessLoopTimeoutTime = ulCurrentTime + ulTimeoutMs;

    /* Call MQTT_ProcessLoop multiple times a timeout happens, or
     * MQTT_ProcessLoop fails. */
    while( ( ulCurrentTime < ulMqttProcessLoopTimeoutTime ) &&
           ( eMqttStatus == MQTTSuccess || eMqttStatus == MQTTNeedMoreBytes ) )
    {
        
        /* Remote drain: every mqtt_response_get_period_ms() publish the whole
         * MQTT FIFO as one message. Independent of the MQTT keep-alive, which
         * MQTT_ProcessLoop handles itself. */
        if( is_connected_to_server &&
            ( xTaskGetTickCount() - mqtt_last_send_tick ) >= pdMS_TO_TICKS( mqtt_response_get_period_ms() ) &&
            !response_fifo_is_empty( &mqtt_response_fifo ) )
        {
            size_t n = response_fifo_drain( &mqtt_response_fifo, mqtt_staging, RESPONSE_FIFO_SIZE );
            mqtt_staging[ n ] = '\0';
            publishToTopic( pMqttContext, MQTT_RESPONSE_TOPIC, mqtt_staging );
            mqtt_last_send_tick = xTaskGetTickCount();
        }
        /* Pairing events (ENROLL / REVOKE / RESET on the local link) go to the
         * backend one at a time from their NVS queue; popped once the publish
         * is accepted by coreMQTT (QoS1, resent by it on reconnect). */
        if( is_connected_to_server &&
            ( xTaskGetTickCount() - pairing_last_send_tick ) >= pdMS_TO_TICKS( 1000 ) &&
            pairing_events_peek( pairing_event_buf, sizeof( pairing_event_buf ) ) )
        {
            if( publishToTopic( pMqttContext, MQTT_PAIRING_TOPIC, pairing_event_buf ) == EXIT_SUCCESS )
            {
                pairing_events_pop();
            }
            pairing_last_send_tick = xTaskGetTickCount();
        }
        eMqttStatus = MQTT_ProcessLoop( pMqttContext );
        ulCurrentTime = pMqttContext->getTime();
        
    }

    return eMqttStatus;
}

// void close_tcp_connection(){
//     /* End TLS session, then close TCP connection. */
//     cleanupESPSecureMgrCerts( &xNetworkContext );
//     ( void ) xTlsDisconnect( &xNetworkContext );
// }

int handleResubscribe( MQTTContext_t * pMqttContext )
{

    int returnStatus = EXIT_SUCCESS;
    MQTTStatus_t mqttStatus = MQTTSuccess;
    BackoffAlgorithmStatus_t backoffAlgStatus = BackoffAlgorithmSuccess;
    BackoffAlgorithmContext_t retryParams;
    uint16_t nextRetryBackOff = 0U;

    assert( pMqttContext != NULL );

    /* Initialize retry attempts and interval. */
    BackoffAlgorithm_InitializeParams( &retryParams,
                                       CONNECTION_RETRY_BACKOFF_BASE_MS,
                                       CONNECTION_RETRY_MAX_BACKOFF_DELAY_MS,
                                       CONNECTION_RETRY_MAX_ATTEMPTS );

    do
    {
        /* Send SUBSCRIBE packet.
         * Note: reusing the value specified in globalSubscribePacketIdentifier is acceptable here
         * because this function is entered only after the receipt of a SUBACK, at which point
         * its associated packet id is free to use. */
        mqttStatus = MQTT_Subscribe( pMqttContext,
                                     pGlobalSubscriptionList,
                                     sizeof( pGlobalSubscriptionList ) / sizeof( MQTTSubscribeInfo_t ),
                                     globalSubscribePacketIdentifier );

        if( mqttStatus != MQTTSuccess )
        {
            CY_LOGE(AWS_IOT_DB, "Failed to send RESUBSCRIBE packet to broker with error = %s.",
                        MQTT_Status_strerror( mqttStatus ) );

            returnStatus = EXIT_FAILURE;
            break;
        }

        CY_LOGI(AWS_IOT_DB, "RESUBSCRIBE sent for topic %.*s to broker.\n\n",
                   MQTT_COMMAND_TOPIC_LENGTH,
                   MQTT_COMMAND_TOPIC );

        /* Process incoming packet. */
        returnStatus = waitForPacketAck( pMqttContext,
                                         globalSubscribePacketIdentifier,
                                         MQTT_PROCESS_LOOP_TIMEOUT_MS );

        if( returnStatus == EXIT_FAILURE )
        {
            break;
        }

        /* Check if recent subscription request has been rejected. globalSubAckStatus is updated
         * in eventCallback to reflect the status of the SUBACK sent by the broker. It represents
         * either the QoS level granted by the server upon subscription, or acknowledgement of
         * server rejection of the subscription request. */
        if( globalSubAckStatus == MQTTSubAckFailure )
        {
            /* Generate a random number and get back-off value (in milliseconds) for the next re-subscribe attempt. */
            backoffAlgStatus = BackoffAlgorithm_GetNextBackoff( &retryParams, generateRandomNumber(), &nextRetryBackOff );

            if( backoffAlgStatus == BackoffAlgorithmRetriesExhausted )
            {
                CY_LOGE(AWS_IOT_DB, "Subscription to topic failed, all attempts exhausted.");
                returnStatus = EXIT_FAILURE;
            }
            else if( backoffAlgStatus == BackoffAlgorithmSuccess )
            { 
                CY_LOGW(AWS_IOT_DB, "Server rejected subscription request. Retrying "
                           "connection after %hu ms backoff.",
                           ( unsigned short ) nextRetryBackOff );

                Clock_SleepMs( nextRetryBackOff );
            }
        }
    } while( ( globalSubAckStatus == MQTTSubAckFailure ) && ( backoffAlgStatus == BackoffAlgorithmSuccess ) );

    return returnStatus;
}

mqtt_identity_t g_mqtt_identity;

static void set_topic(char *dst, size_t dst_size, uint16_t *len_out,
                       const char *base, const char *suffix)
{
    int n = snprintf(dst, dst_size, "%s/%s", base, suffix);
    *len_out = (uint16_t)((n > 0 && (size_t)n < dst_size) ? n : strlen(dst));
}

/* Fills g_mqtt_identity from the provisioned per-device identity (cert + key +
 * endpoint + {sourceTerminalId}/{deviceType}/{deviceId} topic base). Called once
 * at aws_iot_task start, which only runs when provisioning_is_active() — see
 * wifi/station_mode.c and provisioning.h. A completed CSR handshake applies itself via
 * esp_restart(), so the task never has to notice a mid-flight identity change. */
void build_mqtt_identity(void)
{
    char topic_base[128];

    if (!provisioning_is_active())
    {
        CY_LOGE(AWS_IOT_DB, "build_mqtt_identity: device is not provisioned");
        return;
    }

    strncpy(g_mqtt_identity.client_id, g_prov_device_id, sizeof(g_mqtt_identity.client_id) - 1);
    g_mqtt_identity.client_id[sizeof(g_mqtt_identity.client_id) - 1] = '\0';
    g_mqtt_identity.client_id_len = (uint16_t)strlen(g_mqtt_identity.client_id);

    strncpy(g_mqtt_identity.endpoint, g_prov_iot_endpoint, sizeof(g_mqtt_identity.endpoint) - 1);
    g_mqtt_identity.endpoint[sizeof(g_mqtt_identity.endpoint) - 1] = '\0';

    g_mqtt_identity.cert_pem = g_prov_cert_pem;
    g_mqtt_identity.cert_pem_len = strlen(g_prov_cert_pem) + 1;
    g_mqtt_identity.key_pem = g_prov_key_pem;
    g_mqtt_identity.key_pem_len = strlen(g_prov_key_pem) + 1;

    snprintf(topic_base, sizeof(topic_base), "%s/%s/%s", g_prov_source_terminal_id, cy_device_type, g_prov_device_id);

    set_topic(g_mqtt_identity.command_topic, sizeof(g_mqtt_identity.command_topic),
              &g_mqtt_identity.command_topic_len, topic_base, "command");
    set_topic(g_mqtt_identity.get_info_command_topic, sizeof(g_mqtt_identity.get_info_command_topic),
              &g_mqtt_identity.get_info_command_topic_len, topic_base, "getInfoCommand");
    set_topic(g_mqtt_identity.update_fw_command_topic, sizeof(g_mqtt_identity.update_fw_command_topic),
              &g_mqtt_identity.update_fw_command_topic_len, topic_base, "updateFwCommand");
    set_topic(g_mqtt_identity.response_topic, sizeof(g_mqtt_identity.response_topic),
              &g_mqtt_identity.response_topic_len, topic_base, "response");
    set_topic(g_mqtt_identity.get_info_response_topic, sizeof(g_mqtt_identity.get_info_response_topic),
              &g_mqtt_identity.get_info_response_topic_len, topic_base, "getInfoResponse");
    set_topic(g_mqtt_identity.update_fw_response_topic, sizeof(g_mqtt_identity.update_fw_response_topic),
              &g_mqtt_identity.update_fw_response_topic_len, topic_base, "updateFwResponse");
    set_topic(g_mqtt_identity.pairing_topic, sizeof(g_mqtt_identity.pairing_topic),
              &g_mqtt_identity.pairing_topic_len, topic_base, "pairing");

    subscribeTopics[0] = g_mqtt_identity.command_topic;
    subscribeTopics[1] = g_mqtt_identity.get_info_command_topic;
    subscribeTopics[2] = g_mqtt_identity.update_fw_command_topic;
    subscribeTopicLengths[0] = g_mqtt_identity.command_topic_len;
    subscribeTopicLengths[1] = g_mqtt_identity.get_info_command_topic_len;
    subscribeTopicLengths[2] = g_mqtt_identity.update_fw_command_topic_len;
}

void aws_iot_task(void *pvParameters){

    build_mqtt_identity();

    for( ; ; ){

        if(!isConnectedToWifi){
            printf("Wifi is not connected!\r\n");
            sleep( 2 );   
            continue;
        } 
        sleep( 2 );
        MQTTStatus_t status = MQTTSuccess;
        MQTTContext_t mqttContext = { 0 };
        NetworkContext_t xNetworkContext = { 0 };
        int returnStatus = EXIT_SUCCESS;
        bool clientSessionPresent = false;
        bool brokerSessionPresent = false;
        is_connected_to_server = false;

        globalAckPacketIdentifier = 0U;
        globalSubscribePacketIdentifier = 0U;
        globalUnsubscribePacketIdentifier = 0U;

        memset(&outgoingPublishPackets, 0, sizeof(outgoingPublishPackets));
        memset(&pGlobalSubscriptionList, 0, sizeof(pGlobalSubscriptionList));
        memset(&buffer, 0, sizeof(buffer));
        memset(&globalSubAckStatus, 0, sizeof(globalSubAckStatus));
        memset(&pOutgoingPublishRecords, 0, sizeof(pOutgoingPublishRecords));
        memset(&pIncomingPublishRecords, 0, sizeof(pIncomingPublishRecords));
        memset(&xTlsContextSemaphoreBuffer, 0, sizeof(xTlsContextSemaphoreBuffer));

        memset(&mqttContext, 0, sizeof(mqttContext));
        memset(&xNetworkContext, 0, sizeof(xNetworkContext));

        returnStatus = initializeMqtt( &mqttContext, &xNetworkContext );
        
        // disconnectMqttSession(&mqttContext);
        // unsubscribeFromTopic(&mqttContext);
        if( returnStatus == EXIT_SUCCESS )
        {
            for( ; ; )
            {
                returnStatus = connectToServerWithBackoffRetries( &xNetworkContext, &mqttContext, &clientSessionPresent, &brokerSessionPresent );

                if( returnStatus == EXIT_FAILURE )
                {
                    /* Log error to indicate connection failure after all
                    * reconnect attempts are over. */
                    CY_LOGE(AWS_IOT_DB, "Failed to connect to MQTT broker %.*s.",
                                MQTT_ACTIVE_ENDPOINT_LENGTH,
                                MQTT_ACTIVE_ENDPOINT);
                    sleep( MQTT_SUBPUB_LOOP_DELAY_SECONDS );
                }
                else
                {

                    clientSessionPresent = true;
                    if( brokerSessionPresent == true )
                    {
                         
                        // LogInfo("An MQTT session with broker is re-established. "
                        //         "Resending unacked publishes." );
                        /* Handle all the resend of publish messages. */
                        returnStatus = handlePublishResend( &mqttContext );
                    }
                    else
                    {
                        // LogInfo("A clean MQTT connection is established."
                        //         " Cleaning up all the stored outgoing publishes.\n\n" );
                        /* Clean up the outgoing publishes waiting for ack as this new
                        * connection doesn't re-establish an existing session. */
                        cleanupOutgoingPublishes();
                    }
                    is_connected_to_server = true;
                    break;

                }

            }

            if( returnStatus == EXIT_SUCCESS ){

                // CY_LOGI(AWS_IOT_DB,  "Subscribing to the MQTT topics %.*s.",
                //         MQTT_COMMAND_TOPIC_LENGTH,
                //         MQTT_COMMAND_TOPIC );
                returnStatus = subscribeToTopics( &mqttContext );
            }

            if( returnStatus == EXIT_SUCCESS ){
                returnStatus = waitForPacketAck( &mqttContext,
                                                globalSubscribePacketIdentifier,
                                                MQTT_PROCESS_LOOP_TIMEOUT_MS );
            }

            if( ( returnStatus == EXIT_SUCCESS ) && ( globalSubAckStatus == MQTTSubAckFailure ) )
            {
              
                // CY_LOGI(AWS_IOT_DB, "Server rejected initial subscription request. Attempting to re-subscribe to topic %.*s.",
                //         MQTT_COMMAND_TOPIC_LENGTH,
                //         MQTT_COMMAND_TOPIC );
                returnStatus = handleResubscribe( &mqttContext );
            }

            if(returnStatus == EXIT_SUCCESS){

                for( ; ; ){

                    status = processLoopWithTimeout( &mqttContext, MQTT_PROCESS_LOOP_TIMEOUT_MS );  
                    
                    if(status == MQTTRecvFailed){
                        sleep( MQTT_SUBPUB_LOOP_DELAY_SECONDS );
                        break;
                    } 
                    
                }

            }

        }
        xTlsDisconnect ( &xNetworkContext );
    }
    
	// vTaskDelay(1000 / portTICK_PERIOD_MS);
	// xTaskCreate(aws_iot_task, "aws_iot_task", UDP_SERVER_TASK_STACK_DEPTH, (void*)AF_INET, 5, NULL);

    vTaskDelete(NULL);



}