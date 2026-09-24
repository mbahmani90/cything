#pragma once

#include <stdbool.h>

/*
 * Registry of connected TCP clients plus the two producers that queue
 * responses for them (send_data_to_clients / send_raw_to_client) and the
 * task that drains the TCP response FIFO to the sockets. See
 * doc/send-buffers.md.
 */

typedef struct account_struct {
  int  sock;
} account_struct;

extern account_struct account_struct_list[100];

/* send_data_to_clients() target meaning "every paired client": with
 * LOCAL_AUTH_ENFORCE on, sockets that have not authenticated are skipped
 * (they may only be there for a public command); with it off, everyone. */
#define SEND_TO_ALL -2

/* send_data_to_clients() target meaning "every connected client, paired or
 * not" — for the response to a public command (app_command_is_public). */
#define SEND_TO_ALL_PUBLIC -3

/* Max size of one "<sock>:<id>:<data>\n" response line. */
#define RESPONSE_LINE_MAX 256

/* "<sock>" values meaning "every paired client" / "every client". Real lwIP
 * sockets are always > 2 (0-2 are stdio), so neither can be a client. */
#define RESPONSE_TARGET_ALL      0
#define RESPONSE_TARGET_EVERYONE -1

extern int   account_list_size;
extern int   response_id;

void tcp_client_list_init();
int  update_response_id();
void remove_account(int sock);
bool add_account(int sock);
void send_data_to_clients(int sock , const char *data , int data_size);
void send_raw_to_client(int sock , const char *text);
void tcp_response_send_task(void *pvParameters);
