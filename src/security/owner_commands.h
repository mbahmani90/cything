#pragma once

#include <stdbool.h>

/*
 * Management of the paired list and the device password over the local
 * link (doc/local-auth.md, step 4). Owner only: the socket must be
 * authenticated as the paired-list owner AND the line must have arrived in
 * an ENC: frame — checked here, so it holds even with LOCAL_AUTH_ENFORCE off.
 *
 *   app -> LIST
 *   dev -> PAIRED:<b64 userSub>,<b64 installId>,<b64 displayName>,<pairedAt>,<owner|user>   (one per entry)
 *   dev -> PAIREND:<count>
 *
 *   app -> REVOKE:<b64 userSub>,<b64 installId>
 *   dev -> ACK                       or ERR:UNKNOWN | ERR:OWNER (the owner entry) | ERR:BADFMT
 *          Any live session of that phone loses its authentication at once.
 *
 *   app -> PWSET:<b64 newPassword>   (DEVICE_PW_MIN_LEN..DEVICE_PW_MAX_LEN bytes)
 *   dev -> ACK                       or ERR:BADFMT
 *          New salt/verifier; the paired list is untouched. Bootstrap: while
 *          no password is stored yet this is open to any socket — it is how
 *          the first password gets in over TCP (BLE provisioning is the other
 *          way). In that state the line is necessarily plaintext.
 *
 *   app -> UNPAIR                    (any authenticated session — removes the
 *   dev -> ACK                       caller's OWN entry; owner leaving with
 *                                    others promotes entry 0 to owner)
 *
 *   app -> RESET
 *   dev -> ACK
 *          Wipes the paired list and the password; every session (including
 *          the owner's) is de-authenticated. The unit is back to bootstrap.
 *
 *   app -> DISCOVERYMODE:<mode>  (mode is "1" for Wi-Fi, "2" for BLE, "3" for both)
 *   dev -> ACK                   or ERR:BADFMT
 *          Sets the device discovery method. Mode persists in NVS and applies
 *          to how the device advertises: 1=multicast UDP scan only, 2=BLE beacon
 *          only, 3=both methods (default). Beacon advertising is controlled
 *          accordingly (doc/ble-scan-beacon.md).
 *
 * Any of these from a non-owner: ERR:AUTH.
 */

/* `line` is NUL-terminated with its trailing '\n' still attached; `len`
 * excludes the '\n'. Returns true if the line was one of the commands above. */
bool owner_commands_handle_line(char *line, int len, int sock);
