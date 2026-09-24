#include "udp_server.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "device_config/device_config.h"
#include "common/cy_log.h"
#include "udp_socket/udp_response_handler.h"
#include "wifi/station_mode.h"
#include "ble/discovery_mode.h"
#include "device_config/cy_config.h"

#define MULTICAST_TTL 20
#define MULTICAST_IPV6_ADDR "FF02::FC"

#define V4TAG "V4TAG"

/* lwIP cannot unblock a UDP recvfrom from another task (shutdown() is
 * TCP-only), so the server polls with this receive timeout and checks the
 * reconfigure flag on each expiry. */
#define UDP_RECV_TIMEOUT_MS 1000

static volatile bool s_reconfigure = false;

void udp_server_reconfigure(void){
    s_reconfigure = true;
}

/* Discovery only. Wi-Fi pairing (StartP/ssid:/pass:/FinishP) is TCP-only —
 * see tcp_server/pairing.c. Formats the reply (if any) into `reply` and returns its
 * length; 0 means nothing to send. */
int processData(char *rx_buffer , int len , char *reply , size_t reply_size){
	 	
	if(len >= (int)cy_scan_command_len() &&
	   memcmp(rx_buffer, cy_scan_command, cy_scan_command_len()) == 0){     
	
		return udp_get_info_response(reply , reply_size);
	
	}

	return 0;
	
}

void test_udp_multicast_loopback(){
	
	int sock;
    struct sockaddr_in multicast_addr;
    struct ip_mreq mreq;
    char rx_buffer[128];

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
		CY_LOGE(UDP_DB, "Failed to create socket. errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(cy_udp_port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        CY_LOGE(UDP_DB, "Bind failed. errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    inet_aton(cy_multicast_ipv4, &mreq.imr_multiaddr.s_addr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); 
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        CY_LOGE(UDP_DB, "Failed to join multicast group. errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t loop = 0;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_port = htons(cy_udp_port);
    inet_aton(cy_multicast_ipv4, &multicast_addr.sin_addr);

    while (1) {
      
        char msg[] = "Hello Multicast Loopback!";
        int err = sendto(sock, msg, strlen(msg), 0,
                         (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
        if (err < 0) {
            CY_LOGE(UDP_DB, "Error sending multicast: errno %d", errno);
        } else {
            CY_LOGI(UDP_DB, "Sent multicast: %s", msg);
        }

        
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len > 0) {
            rx_buffer[len] = 0;
            CY_LOGI(UDP_DB, "Received multicast (loopback): %s", rx_buffer);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    close(sock);
    vTaskDelete(NULL);
}

int socket_add_ipv4_multicast_group(int sock)
{
    struct ip_mreq imreq = { 0 };
    int err = 0;
    imreq.imr_interface.s_addr = IPADDR_ANY;
    err = inet_aton(cy_multicast_ipv4, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV4 multicast address '%s' is invalid.", cy_multicast_ipv4);
        err = -1;
        return err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", cy_multicast_ipv4);
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        return err;
    }

    return 0;
}

int socket_add_multicast_ipv6_group(int sock , int netif_index){
	struct ipv6_mreq v6imreq = { 0 };
	int err = 0;
	
    err = inet6_aton(MULTICAST_IPV6_ADDR, &v6imreq.ipv6mr_multiaddr);
    if (err != 1) {
        ESP_LOGE(TAG, "Configured IPV6 multicast address '%s' is invalid.", MULTICAST_IPV6_ADDR);
        return err;
    }
    ESP_LOGI(TAG, "Configured IPV6 Multicast address %s", inet6_ntoa(v6imreq.ipv6mr_multiaddr));
    
	ip6_addr_t multi_addr;
    inet6_addr_to_ip6addr(&multi_addr, &v6imreq.ipv6mr_multiaddr);
    if (!ip6_addr_ismulticast(&multi_addr)) {
        ESP_LOGW(TAG, "Configured IPV6 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV6_ADDR);
    }

    v6imreq.ipv6mr_interface = (unsigned int)netif_index;
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                     &v6imreq, sizeof(struct ipv6_mreq));
    if (err < 0) {
        ESP_LOGE(TAG, "Failed to set IPV6_ADD_MEMBERSHIP. Error %d", errno);
        return err;
    }
	
	return err;

}

int create_udp_socket(int *sock){

	struct sockaddr_in6 dest_addr;
	dest_addr.sin6_family = AF_INET6;
	dest_addr.sin6_port = htons(cy_udp_port);
	bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
	int err = -1;
	
	*sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
	if (*sock < 0) {
		CY_LOGE(UDP_DB, "Unable to create socket: errno %d", errno);
		goto error_line;
	}
 		
	CY_LOGI(UDP_DB, "Socket created");

	int reuse = 1;
	setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	int v6only = 0;
	setsockopt(*sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

	err = bind(*sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err < 0) {
		CY_LOGE(UDP_DB, "Socket unable to bind: errno %d", errno);
		goto error_line;			
	}
	
	int netif_index = esp_netif_get_netif_impl_index(p_netif_sta);
	if(netif_index < 0) {
		CY_LOGE(UDP_DB, "Failed to get netif index");
		goto error_line;
	}

	err = setsockopt(*sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &netif_index, sizeof(unsigned int));
	if (err < 0) {
		CY_LOGE(UDP_DB, "Failed to set IPV6_MULTICAST_IF. Error %d", errno);
		goto error_line;
	}

	uint8_t ttl = MULTICAST_TTL;
	setsockopt(*sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(uint8_t));
	if (err < 0) {
		CY_LOGE(UDP_DB, "Failed to set IPV6_MULTICAST_HOPS. Error %d", errno);
		goto error_line;
	}

	struct timeval tv = { .tv_sec = UDP_RECV_TIMEOUT_MS / 1000, .tv_usec = (UDP_RECV_TIMEOUT_MS % 1000) * 1000 };
	setsockopt(*sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* The multicast groups are what the app's broadcast scan hits. In BLE-only
	 * discovery mode we stay out of them: the socket still answers the unicast
	 * GET_INFO the scan beacon leads to (doc/ble-scan-beacon.md). */
	if(discovery_mode_should_scan_wifi()){
		err = socket_add_multicast_ipv6_group(*sock , netif_index);
		if (err < 0) {
			CY_LOGE(UDP_DB, "Failed to add multicast IPv6 group. Error %d", errno);
			goto error_line;
		}

		err = socket_add_ipv4_multicast_group(*sock);
		if (err < 0) {
			CY_LOGE(UDP_DB, "Failed to add multicast IPv4 group. Error %d", errno);
			goto error_line;
		}
	}else{
		CY_LOGI(UDP_DB, "discovery mode BLE: multicast scan disabled, unicast only");
	}
	
	return err;

error_line:
	if(err < 0 && *sock != -1){
		close(*sock);
		*sock = -1;
	}
	return err;
}

void udp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str [128];
    char reply    [UDP_REPLY_MAX];
    
	int sock = -1;

    while (1) {

		if (create_udp_socket(&sock) < 0) {
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			continue;
		}

		CY_LOGI(UDP_DB, "Waiting for data");
        while (1) {
			struct sockaddr_storage source_addr;
			socklen_t socklen = sizeof(source_addr);
			int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

			if (len < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					if (s_reconfigure) {
						s_reconfigure = false;
						CY_LOGI(UDP_DB, "discovery mode changed: rebuilding socket");
						break;
					}
					continue;
				}
				CY_LOGE(UDP_DB, "recvfrom failed: errno %d", errno);
				break;
			}
			else {
				if (source_addr.ss_family == PF_INET) {
					inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
				} else if (source_addr.ss_family == PF_INET6) {
					inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
				}

				rx_buffer[len] = 0; 
			
				unsigned char mac_str[20];
				CY_LOGI(UDP_DB, "Received %d bytes from %s:", len, addr_str);
				CY_LOGI(UDP_DB, "%s", rx_buffer);
				esp_base_mac_addr_get(mac_str);
				CY_LOGI(UDP_DB, "BASE: %02X %02X %02X %02X %02X %02X", mac_str[0], mac_str[1], mac_str[2], mac_str[3], mac_str[4], mac_str[5]);
				esp_efuse_mac_get_default(mac_str);				
				CY_LOGI(UDP_DB, "EFUSE: %02X %02X %02X %02X %02X %02X", mac_str[0], mac_str[1], mac_str[2], mac_str[3], mac_str[4], mac_str[5]);
				int reply_len = processData(rx_buffer , len , reply , sizeof(reply));
				// Deepest path of this task (recvfrom -> processData -> udp_get_info_response).
				CY_LOGI(UDP_DB, "udp_server stack high-water mark: %u bytes free",
						(unsigned)uxTaskGetStackHighWaterMark(NULL));
	
				if(reply_len > 0){						
					int err = sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
					
					if (err < 0) {
						CY_LOGE(UDP_DB, "Error occured during sending: errno %d", errno);
						break;
					}

				}

			}

        }

        if (sock != -1) {
            CY_LOGE(UDP_DB, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }

    vTaskDelete(NULL);
	
}