#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Per-device X.509 certificate provisioning — the on-chip half of the CSR
 * handshake in ../../CypressTerminalKmp85327491/doc/device-pairing.md
 * ("The CSR handshake") and ~/.claude/plans/memoized-snuggling-diffie.md
 * ("Firmware contract").
 *
 * A device only connects to AWS IoT once provisioning_is_active() — it needs
 * the per-device cert + key from the CSR handshake (there is no compiled-in
 * shared-fleet cert). An unprovisioned unit stays off the broker
 * (wifi/station_mode.c gates aws_iot_task on it). When PFIN lands, the device
 * persists the new identity to NVS and esp_restart()s straight into it — see
 * aws_iot_handler.c build_mqtt_identity().
 *
 * `sourceTerminalId` (topic segment 1) is the ROOT channel id — the app sends the
 * same value for every forked copy of a channel, so co-users on different
 * forks reach this device on the same topic. The device just stores and echoes
 * it; it never needs the per-user channelId. See the plan's
 * "Topic-scheme revision".
 */

#define PROV_DEVICE_ID_MAX   40
#define PROV_SOURCE_TERMINAL_ID_MAX  40
#define PROV_ENDPOINT_MAX    128
#define PROV_CERT_PEM_MAX    2048
#define PROV_KEY_PEM_MAX     512

/* Loaded from NVS at boot by provisioning_init(); valid only once
 * provisioning_is_active() is true. */
extern char g_prov_device_id[PROV_DEVICE_ID_MAX];
extern char g_prov_source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
extern char g_prov_iot_endpoint[PROV_ENDPOINT_MAX];
extern char g_prov_cert_pem[PROV_CERT_PEM_MAX];
extern char g_prov_key_pem[PROV_KEY_PEM_MAX];

/* Reads any previously-completed provisioning out of NVS. Call once at boot,
 * before aws_iot_task starts (see app_main.c). Idempotent. */
void provisioning_init(void);

/* True once a full CSR handshake has completed and the g_prov_* identity
 * above is valid. aws_iot_handler.c's build_mqtt_identity() switches to it
 * instead of the compiled-in shared fleet cert when this is true. */
bool provisioning_is_active(void);

/* Fields for the local UDP scan reply (see udp_socket/udp_response_handler.c) — the new
 * 9-field CSV needs sourceTerminalId / deviceId / provisionState. Before a
 * handshake completes these come back as "" / "" / "unprovisioned"; the general
 * app device list won't recognize that (by design — only a dedicated "add a
 * device" flow should act on an unprovisioned reply), which is fine, this
 * device stays reachable under its OLD identity in the meantime. */
void provisioning_get_scan_fields(char *source_terminal_id_out, size_t source_terminal_id_out_size,
                                   char *device_id_out, size_t device_id_out_size,
                                   char *provision_state_out, size_t provision_state_out_size);

/*
 * Dispatches one already-line-split TCP command into the provisioning state
 * machine (PROV:/CSRREQ/DEVID:/CERT:<seq>:<total>:<b64>/PFIN — see
 * doc/device-pairing.md). Sends its own ACK/response(s) on `sock` directly
 * rather than through send_raw_to_client(), because CLAIM:/CSR: lines can
 * exceed RESPONSE_LINE_MAX. Returns true if `line` was recognized as one of
 * these commands (whether or not it was valid in the current state), so the
 * caller (tcp_server/tcp_command.c) knows not to fall through to its other handlers.
 *
 * On a successful PFIN, persists the new identity to NVS and calls
 * esp_restart() after acking — see the module doc comment above for why a
 * reboot, not a live reconnect, applies the new identity.
 */
bool provisioning_handle_tcp_line(char *line, int line_len, int sock);
