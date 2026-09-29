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
#include "device_config/cy_config.h"

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

/* One dual-stack socket on [::]:cy_udp_port. Unicast only: the app finds
 * the device over mDNS or the BLE scan beacon and sends GET_INFO straight to
 * its address (doc/udp-discovery.md). */
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