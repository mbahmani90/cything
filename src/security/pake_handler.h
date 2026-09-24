#pragma once

#include <stdbool.h>

/*
 * Password pairing over the local link: SRP-6a wrapped in the line protocol
 * (doc/local-auth.md, step 2). esp_srp does the maths; this file only moves
 * bytes between the socket and it, and keeps the per-socket state in the
 * connection's local_session_t.
 *
 *   app -> PAKE1:<b64 A>
 *   dev -> PAKE2:<b64 salt>,<b64 B>        or ERR:NOPW | ERR:LOCKED,<s> | ERR:BADFMT
 *   app -> PAKE3:<b64 M1>
 *   dev -> PAKE4:<b64 M2>                  or ERR:BADPW | ERR:SEQ | ERR:BADFMT
 *
 * Group: 3072-bit (RFC 5054), hash SHA-512, identity DEVICE_PW_SRP_IDENTITY
 * — fixed by esp_srp; the phone must use the same. After PAKE4 the session
 * holds K_pake = the first 32 bytes of the 64-byte SRP session key and is
 * in state LS_PAKE_OK; the only things that may follow on that socket are
 * ENROLL (local_auth.c) inside ENC: frames (enc_frame.c).
 *
 * Every failed proof feeds device_password_note_failure() (rate limiting);
 * a success clears the counters for that source IP.
 */

/* `line` is NUL-terminated with its trailing '\n' still attached; `len`
 * excludes the '\n'. Returns true if the line was a PAKE command (whether
 * or not it was valid), so the dispatcher stops there. */
bool pake_handle_line(char *line, int len, int sock);

/* Send "<text>\n" straight to `sock`, bypassing the response FIFO. Long
 * handshake replies (B is 384 bytes) do not fit RESPONSE_LINE_MAX; the
 * provisioning handshake does the same. Shared with local_auth.c. */
void security_send_line(int sock, const char *text);
