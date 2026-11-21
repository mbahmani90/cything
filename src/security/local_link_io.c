#include "local_link_io.h"

#include "lwip/sockets.h"
#include "ble/ble_app_gatt.h"

void local_link_send(int sock, const void *data, size_t len){
    if(local_link_is_ble(sock)){
        ble_app_gatt_link_send(sock, data, len);
    }else{
        send(sock, data, len, 0);
    }
}
