#include "udp_response_handler.h"

#include <stdio.h>
#include <string.h>
#include "esp_netif.h"

#include "device_config/device_config.h"
#include "aws/claim_credentials.h"
#include "security/device_password.h"
#include "common/cy_log.h"
#include "aws/provisioning.h"
#include "wifi/wifi_info_handler.h"
#include "device_config/cy_config.h"

/* "a.b.c.d" of the active interface; `device_wifi_info` must hold 16 bytes. */
void get_device_wifi_info(char *device_wifi_info){
	sprintf(device_wifi_info , IPSTR , IP2STR(&device_ip));
}

// Local scan-reply CSV — see doc/device-pairing.md "Scan reply (UDP)" and
// aws/provisioning.h. Always the new 9-field per-device-cert format:
// ip, sourceTerminalId, deviceName, deviceType, deviceId, provisionState,
// firmwareVersion, hardwareVersion, provisioningCaps. sourceTerminalId (topic
// segment 1 = the root channel id) and deviceId come back blank with
// provisionState "unprovisioned" until a CSR handshake completes — this device
// stays reachable under its OLD identity via AWS IoT in the meantime (see
// aws/provisioning.h's module doc comment); only the app's general device-list
// matching (which needs a real sourceTerminalId) is affected, not connectivity.
int udp_get_info_response(char *reply , size_t reply_size){

    char device_wifi_info[64];
    get_device_wifi_info(device_wifi_info);
	CY_LOGI(UDP_DB, "udp_get_info_response");

	char source_terminal_id[PROV_SOURCE_TERMINAL_ID_MAX];
	char device_id[PROV_DEVICE_ID_MAX];
	char provision_state[16];
	provisioning_get_scan_fields(source_terminal_id, sizeof(source_terminal_id),
		device_id, sizeof(device_id), provision_state, sizeof(provision_state));

	/* provisioningCaps (field 9): '|'-separated tokens the app tests with
	 * contains(). No ',' — it is a CSV field.
	 *   claim    run the REQID:/CLAIM: claim-attestation step (slice CC). Only
	 *            when claim material is actually flashed, so the app doesn't
	 *            wait on a CLAIM: reply we can't send.
	 *   pake     this firmware speaks the password pairing handshake
	 *            (security/pake_handler.h): pair with PAKE1..4 + ENROLL, then
	 *            AUTH1..3 on every connection, instead of connecting bare.
	 *   nopw     no device password stored: open to all. PAKE uses the
	 *            model's initial password (cy_initial_password); the first
	 *            phone to ENROLL becomes the owner and sets a real one.
	 *   authreq  LOCAL_AUTH_ENFORCE is on AND a password is set: protected
	 *            commands need a paired phone and arrive encrypted; plaintext
	 *            use is refused. Not advertised on an open (no-password) device
	 *            — there is nothing to enforce, so it behaves openly. */
	char provisioning_caps[40] = "pake";
	if(claim_credentials_present())   strlcat(provisioning_caps, "|claim",   sizeof(provisioning_caps));
	if(!device_password_is_set())     strlcat(provisioning_caps, "|nopw",    sizeof(provisioning_caps));
#if LOCAL_AUTH_ENFORCE == 1
	if(device_password_is_set())      strlcat(provisioning_caps, "|authreq", sizeof(provisioning_caps));
#endif

	int len = snprintf(reply , reply_size , "%s,%s,%s,%s,%s,%s,%s,%s,%s" , device_wifi_info,
		source_terminal_id, cy_device_name, cy_device_type, device_id, provision_state,
		cy_firmware_version, cy_hardware_version, provisioning_caps);
	if(len >= (int)reply_size){
		CY_LOGW(UDP_DB, "scan reply truncated (%d >= %u)", len, (unsigned)reply_size);
		len = reply_size - 1;
	}

	CY_LOGI(UDP_DB, "%s", reply);
	return len;

}
