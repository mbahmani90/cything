#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Wi-Fi pairing over TCP: StartP -> ssid:<ssid>\n -> pass:<pw>\n -> FinishP,
 * each accepted step answered with "ACK\n". On FinishP the router
 * credentials are written to the wifi_info partition and the device reboots
 * into station mode. See doc/pairing.md.
 *
 * One state machine for the whole device (not per connection), guarded by a
 * one-shot inactivity timer (PAIRING_TIMEOUT_MS, wifi/wifi_config.h).
 *
 * The final step is shared with BLE pairing (ble/ble_pairing.c): both
 * transports collect an SSID + password their own way and end in
 * pairing_commit().
 */

/* Write `ssid` / `password` (NUL-terminated) and station mode to the
 * wifi_info partition, then reboot 1 s later (reboot_after_ms). Returns the
 * flash error and does NOT reboot if the write failed. Safe to call from any
 * task; the caller should send its "done" reply right after. */
esp_err_t pairing_commit(const char *ssid, const char *password);

/* Call once from app_main() before the TCP server starts. */
void pairing_timer_init(void);

/* Feed one NUL-terminated line (trailing '\n' still attached). Returns true
 * if the line was a pairing command — whether or not it was valid for the
 * current step — so the dispatcher knows not to try other handlers. Replies
 * are queued on `sock` via send_raw_to_client(). */
bool pairing_handle_line(const char *line, int sock);
