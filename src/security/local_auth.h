#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Enrollment and reconnect authentication on the local link
 * (doc/local-auth.md, steps 3–4). All keys are 32 bytes; MACs are
 * HMAC-SHA256; base64 on the wire.
 *
 * Enroll — only on a socket that has just proven the password (LS_PAKE_OK):
 *
 *   app -> ENROLL:<b64 userSub>,<b64 displayName>,<b64 installId>
 *   dev -> ENROLLED:<b64 K_phone>,<owner|user>     or ERR:SEQ | ERR:FULL | ERR:BADFMT
 *
 *   K_phone is fresh from the hardware RNG and stored in the paired list;
 *   the first entry ever is the owner. The socket stays on K_pake.
 *
 * Reconnect — on a fresh socket (LS_NEW), no password involved:
 *
 *   app -> AUTH1:<b64 userSub>,<b64 installId>,<b64 N_p>        N_p: 16 random bytes
 *   dev -> AUTH2:<b64 N_d>,<b64 HMAC(K_phone, "dev" ‖ N_p ‖ N_d)>   or ERR:UNKNOWN | ERR:BADFMT
 *   app -> AUTH3:<b64 HMAC(K_phone, "phn" ‖ N_d ‖ N_p)>
 *   dev -> AUTHOK:<owner|user>                                    or ERR:BADAUTH | ERR:SEQ
 *
 *   The device proves first, so a phone never hands its proof to an
 *   impostor; the "dev"/"phn" labels keep the two proofs distinct. After
 *   AUTHOK the socket holds
 *     K_session = HKDF-SHA256(ikm = K_phone, salt = N_p ‖ N_d, info = "cy-local-v1")
 *   and is in LS_AUTH_OK. ERR:UNKNOWN is what a revoked phone sees.
 *
 * Until enc_frame.c (step 6) lands these lines travel in the clear —
 * acceptable on a dev bench only; K_phone must never cross the wire
 * unencrypted on a shipped build.
 */

#define LOCAL_AUTH_NONCE_LEN   16
#define LOCAL_AUTH_MAC_LEN     32
#define LOCAL_AUTH_HKDF_INFO   "cy-local-v1"

/* `line` is NUL-terminated with its trailing '\n' still attached; `len`
 * excludes the '\n'. Returns true if the line was an ENROLL / AUTH command. */
bool local_auth_handle_line(char *line, int len, int sock);

/* HMAC-SHA256 and HKDF-SHA256 (extract + one expand block, 32-byte output),
 * on mbedtls_md so they build under both ESP-IDF and the Arduino prebuilt
 * mbedtls regardless of its HKDF config. Shared with enc_frame.c. */
void local_auth_hmac(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len, uint8_t out[LOCAL_AUTH_MAC_LEN]);
void local_auth_hkdf32(const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *salt, size_t salt_len,
                       const char *info, uint8_t out[32]);

/* Constant-time equality. */
bool local_auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);
