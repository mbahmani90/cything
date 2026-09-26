#pragma once

#include <stdbool.h>

/*
 * Management of the paired list and the device password over the local
 * link (doc/local-auth.md, step 4). Owner only: the socket must be
 * authenticated as the paired-list owner AND the line must have arrived in
 * an ENC: frame — checked here, so it holds even with LOCAL_AUTH_ENFORCE off.
 *
 *   app -> LIST
 *   dev -> PAIRED:<b64 userSub>,<b64 userEmail>,<pairedAt>,<owner|user>   (one per account)
 *   dev -> PAIREND:<count>
 *
 *   (userEmail is the label the app sent at ENROLL — empty if none — so the
 *   owner can tell accounts apart; userSub is what REVOKE takes.)
 *
 *   app -> REVOKE:<b64 userSub>[,<b64 installId>]   (installId ignored)
 *   dev -> ACK                       or ERR:UNKNOWN | ERR:OWNER (the owner entry) | ERR:BADFMT
 *          Every live session of that account loses its authentication at once.
 *
 *   app -> PWSET:<b64 newPassword>   (DEVICE_PW_MIN_LEN..DEVICE_PW_MAX_LEN bytes)
 *   dev -> ACK                       or ERR:BADFMT
 *          New salt/verifier; every non-owner account is revoked and the
 *          owner account's key is rotated (re-ENROLL to get the new one). Owner-only
 *          even on an open device: there the first phone PAKEs with the
 *          initial password and ENROLLs (becoming owner) before this.
 *
 *   app -> UNPAIR                    (any authenticated session — removes the
 *   dev -> ACK                       caller's OWN account entry, so all its
 *          or ERR:OWNER              phones. The owner is refused and stays
 *                                    paired: ownership only ends on RESET /
 *                                    power-cycle factory reset)
 *
 *   app -> RESET
 *   dev -> ACK
 *          Wipes the paired list and the password; every session (including
 *          the owner's) is de-authenticated. The unit is back to open.
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
