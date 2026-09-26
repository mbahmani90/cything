#pragma once

#include <stdbool.h>

/*
 * Who may send what on the local link (doc/local-auth.md, "Access policy").
 * Decided per COMMAND, not per frame: a keyed socket always encrypts
 * (enc_frame.c); this file decides whether a line needs a paired phone
 * behind it at all.
 *
 *   handshake  PAKE1/PAKE3, AUTH1/AUTH3, ENROLL — always accepted; their
 *              handlers check the session state themselves.
 *   public     GET_INFO, plus whatever the application's
 *              app_command_is_public() hook says — accepted from any socket,
 *              plaintext or encrypted, no pairing needed.
 *   protected  everything else — needs an authenticated socket (LS_AUTH_OK,
 *              or LS_PAKE_OK once enrolled) AND must arrive in an ENC: frame.
 *
 * Open device: while no password is stored (fresh unit, after RESET, or after
 * PWCLEAR) every line is public. PAKE still runs, with the model's initial
 * password, so the first phone can ENROLL as owner and PWSET encrypted —
 * PWSET: is owner-only in every state (owner_commands.c).
 *
 * LOCAL_AUTH_ENFORCE (device_config.h) = 0 keeps today's behaviour: every
 * line is accepted and SEND_TO_ALL reaches every socket. The handshake still
 * works, so the app can be rolled out first and the flag flipped after.
 */

/* Called by tcp_dispatch_line() before any handler. Returns true if the
 * line may proceed; otherwise a plaintext "ERR:AUTH" (or "ERR:ENC" for a
 * protected line sent in the clear on a keyed socket) has been sent and
 * the line must be dropped. */
bool access_policy_allow(const char *line, int len, int sock);

/* True if `sock` has proven it belongs to a paired phone: LS_AUTH_OK, or
 * LS_PAKE_OK after ENROLL. Broadcasts (SEND_TO_ALL) are limited to these
 * sockets when LOCAL_AUTH_ENFORCE is on. */
bool access_policy_sock_authenticated(int sock);
