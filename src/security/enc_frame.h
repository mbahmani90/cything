#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Encrypted line framing for the local link (doc/local-auth.md, "Session
 * encryption"). Once a socket holds a key — K_pake after PAKE4, K_session
 * after AUTHOK — every line in both directions is
 *
 *     ENC:<b64 nonce12>,<b64 AES-256-GCM(key, nonce12, line) ‖ tag16>\n
 *
 * where `line` is the original line WITHOUT its trailing '\n'. The nonce is
 * a 4-byte direction tag ‖ 64-bit big-endian counter that starts at 1 and
 * only goes up; the receiver rejects a counter that is not greater than the
 * last one it accepted, so a recorded frame cannot be replayed and a frame
 * cannot be reflected back to its sender.
 *
 *     app -> dev   direction 0x00000001
 *     dev -> app   direction 0x00000002
 *
 * Receive: tcp_client_recv_task hands every complete line to
 * enc_frame_unwrap() before dispatch. Send: every byte to a keyed socket
 * goes through enc_frame_send(); plaintext sockets are passed straight to
 * send(). Handshake replies that precede the key (PAKE2/PAKE4/AUTH2/AUTHOK)
 * are sent before the state moves, so they stay plaintext by construction.
 */

#define ENC_FRAME_NONCE_LEN   12
#define ENC_FRAME_TAG_LEN     16
#define ENC_FRAME_DIR_TO_DEV  0x00000001u
#define ENC_FRAME_DIR_TO_APP  0x00000002u

/* Largest plaintext line enc_frame_send() will encrypt. */
#define ENC_FRAME_PLAIN_MAX   768

/* Call once from cything_begin(). */
void enc_frame_init(void);

/* If `line` (NUL-terminated, trailing '\n' attached, `len` excluding it) is
 * an ENC: frame for a keyed session, decrypt it IN PLACE: on return `line`
 * holds the plaintext with '\n' re-attached and NUL-terminated, `*len` its
 * length excluding the '\n', and the session's rx_encrypted flag is set.
 * Returns false and sends ERR:ENC if the frame is malformed, replayed or
 * fails authentication — the caller drops the line. A non-ENC line on any
 * socket is returned untouched with rx_encrypted cleared (the access policy
 * in tcp_command.c decides whether that is allowed). */
bool enc_frame_unwrap(char *line, int *len, int sock, size_t capacity);

/* Send `payload` (which may end in '\n') to `sock`, encrypting it if the
 * socket holds a key, else raw. Serialised per socket. */
void enc_frame_send(int sock, const char *payload, size_t payload_len);
