#include "tcp_server.h"

#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "device_config/device_config.h"
#include "common/cy_log.h"
#include "common/task_config.h"
#include "tcp_server/tcp_client_list.h"
#include "tcp_server/tcp_client_recv.h"
#include "security/local_session.h"
#include "device_config/cy_config.h"

void tcp_server_task(void *pvParameters)
{
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_storage dest_addr;
    int listen_sock;

    while(1){

        if (addr_family == AF_INET) {
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(cy_tcp_port);
            ip_protocol = IPPROTO_IP;
        }

        listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0) {
            CY_LOGE(TCP_SERVER_DB, "Unable to create socket: errno %d", errno);
    
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        CY_LOGI(TCP_SERVER_DB, "Socket created");
        int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            CY_LOGE(TCP_SERVER_DB, "Socket unable to bind: errno %d", errno);
            CY_LOGE(TCP_SERVER_DB, "IPPROTO: %d", addr_family);
        //    goto CLEAN_UP;
        }
        CY_LOGI(TCP_SERVER_DB, "Socket bound, port %d", (int)cy_tcp_port);

        err = listen(listen_sock, 10);
        if (err != 0) {
            CY_LOGE(TCP_SERVER_DB, "Error occurred during listen: errno %d", errno);
        //    goto CLEAN_UP;
        }
        while (1) {
            CY_LOGI(TCP_SERVER_DB, "Socket listening");
            struct sockaddr_storage source_addr; 
            socklen_t addr_len = sizeof(source_addr);
            int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0) {
                // Out of sockets / memory (errno 23 = ENFILE) is transient: keep
                // this listening socket and try again. Leaving the loop used to
                // open a NEW listening socket without closing this one, leaking
                // one socket per failure until none were left.
                CY_LOGE(TCP_SERVER_DB, "Unable to accept connection: errno %d", errno);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            if (TCP_SERVER_DB) {
                char addr_str[128] = "";
                if (source_addr.ss_family == PF_INET) {
                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
                }
                CY_LOGI(TCP_SERVER_DB, "Socket accepted ip address: %s", addr_str);
            }
            uint32_t peer_ip = 0;
            if (source_addr.ss_family == PF_INET) {
                peer_ip = ((struct sockaddr_in *)&source_addr)->sin_addr.s_addr;
            }

            // At the cap: the same phone reconnecting replaces its own older
            // connection(s) (their recv tasks clean up); anyone else is refused.
            if(local_session_count() >= TCP_CLIENT_MAX && local_session_shutdown_peer(peer_ip) == 0){
                CY_LOGW(TCP_SERVER_DB, "%d connections already, rejecting", TCP_CLIENT_MAX);
                shutdown(sock, 0);
                close(sock);
                continue;
            }

            int ka = 1, ka_idle = TCP_KEEPALIVE_IDLE_S, ka_intvl = TCP_KEEPALIVE_INTVL_S, ka_cnt = TCP_KEEPALIVE_CNT;
            setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,  &ka,       sizeof(ka));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle,  sizeof(ka_idle));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,   sizeof(ka_cnt));

            // Registry or session pool full: refuse the client instead of
            // leaving an unread socket open.
            if(!add_account(sock)){
                CY_LOGW(TCP_SERVER_DB, "Client list full, rejecting connection");
                shutdown(sock, 0);
                close(sock);
                continue;
            }
            local_session_t *session = local_session_acquire(sock, peer_ip);
            if(session == NULL){
                remove_account(sock);
                shutdown(sock, 0);
                close(sock);
                continue;
            }
            // The session slot never moves, so the task can keep this pointer
            // for the life of the connection (account_struct_list is compacted
            // on every disconnect, which is why it is not passed instead).
            // No heap for the task: undo, or the socket and its slot would
            // be held forever with nothing to read or close them.
            if(xTaskCreate(tcp_client_recv_task, "tcp_client_recv", TCP_SERVER_RECV_TASK_STACK_SIZE,
                           (void *)session , TCP_SERVER_RECV_TASK_PRIORITY, NULL) != pdPASS){
                CY_LOGE(TCP_SERVER_DB, "No memory for a client task, rejecting connection");
                local_session_release(sock);
                remove_account(sock);
                shutdown(sock, 0);
                close(sock);
                continue;
            }

        }
        
    }

    vTaskDelete(NULL);

}

