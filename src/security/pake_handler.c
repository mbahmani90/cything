#include "pake_handler.h"

#include <stdio.h>
#include <string.h>
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "esp_srp.h"

#include "common/cy_log.h"
#include "security/local_session.h"
#include "security/device_password.h"
#include "security/enc_frame.h"

#define PAKE1_PREFIX   "PAKE1:"
#define PAKE3_PREFIX   "PAKE3:"

#define SRP_BYTES_MAX   384   /* 3072-bit group: A, B, verifier */
#define SRP_PROOF_LEN   64    /* SHA-512 */
#define B64_MAX(n)      ((((n) + 2) / 3) * 4 + 1)

void security_send_line(int sock, const char *text){
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    if(n < (int)sizeof(buf)){
        enc_frame_send(sock, buf, n);
    }else{
        /* Long handshake lines (PAKE2) are always pre-key, so plaintext. */
        send(sock, text, strlen(text), 0);
        send(sock, "\n", 1, 0);
    }
}

static void send_err(int sock, const char *code){
    char buf[32];
    snprintf(buf, sizeof(buf), "ERR:%s", code);
    security_send_line(sock, buf);
}

/* Decode the base64 payload after `prefix` into `out`. Returns the decoded
 * length, or -1 if it is malformed or does not fit. */
static int decode_payload(const char *line, int len, const char *prefix,
                          uint8_t *out, size_t out_size){
    int plen = strlen(prefix);
    if(len <= plen) return -1;
    size_t n = 0;
    int rc = mbedtls_base64_decode(out, out_size, &n,
                                   (const unsigned char *)line + plen, (size_t)(len - plen));
    if(rc != 0 || n == 0) return -1;
    return (int)n;
}

/* Abandon an in-progress or failed exchange: free the SRP context, wipe the
 * key, back to LS_NEW. Leaves an established session (PAKE_OK and later)
 * alone — the caller checks state first. */
static void abort_pake(local_session_t *s){
    if(s->srp != NULL){
        esp_srp_free((esp_srp_handle_t *)s->srp);
        s->srp = NULL;
    }
    memset(s->key, 0, sizeof(s->key));
    s->state = LS_NEW;
}

/* PAKE1:<b64 A> -> PAKE2:<b64 salt>,<b64 B> */
static void handle_pake1(local_session_t *s, const char *line, int len){

    /* A fresh PAKE1 abandons whatever this socket was doing — an in-progress
     * SRP, an authenticated session, or an AUTH the client gave up on — and
     * starts over. See local_session_reset_handshake(). */
    local_session_reset_handshake(s);

    uint32_t lock = device_password_lock_remaining_s(s->peer_ip);
    if(lock > 0){
        char buf[24];
        snprintf(buf, sizeof(buf), "LOCKED,%u", (unsigned)lock);
        send_err(s->sock, buf);
        return;
    }

    uint8_t A[SRP_BYTES_MAX];
    int len_A = decode_payload(line, len, PAKE1_PREFIX, A, sizeof(A));
    if(len_A < 0){
        send_err(s->sock, "BADFMT");
        return;
    }

    const uint8_t *salt, *ver;
    size_t salt_len, ver_len;
    if(!device_password_get(&salt, &salt_len, &ver, &ver_len)){
        send_err(s->sock, "NOPW");
        return;
    }

    esp_srp_handle_t *hd = esp_srp_init(ESP_NG_3072);
    if(hd == NULL){
        send_err(s->sock, "NOMEM");
        return;
    }

    char    *B = NULL;      int      len_B = 0;
    char    *K = NULL;      uint16_t len_K = 0;
    esp_err_t err = esp_srp_set_salt_verifier(hd, (const char *)salt, (int)salt_len,
                                              (const char *)ver, (int)ver_len);
    if(err == ESP_OK) err = esp_srp_srv_pubkey_from_salt_verifier(hd, &B, &len_B);
    /* esp_srp rejects A == 0 mod N here (the classic forced-zero-secret attack). */
    if(err == ESP_OK) err = esp_srp_get_session_key(hd, (char *)A, len_A, &K, &len_K);
    if(err != ESP_OK || B == NULL || K == NULL || len_K < sizeof(s->key) || len_B > SRP_BYTES_MAX){
        CY_LOGW(TCP_SERVER_DB, "pake: setup failed: %s", esp_err_to_name(err));
        esp_srp_free(hd);
        send_err(s->sock, "BADFMT");
        return;
    }

    /* Reply: PAKE2:<b64 salt>,<b64 B>. B is owned by the handle; encode
     * straight into the line buffer. */
    static const char prefix[] = "PAKE2:";
    char reply[sizeof(prefix) + B64_MAX(DEVICE_PW_SALT_LEN) + 1 + B64_MAX(SRP_BYTES_MAX)];
    size_t pos = 0, n = 0;
    memcpy(reply, prefix, sizeof(prefix) - 1);
    pos = sizeof(prefix) - 1;
    mbedtls_base64_encode((unsigned char *)reply + pos, sizeof(reply) - pos, &n, salt, salt_len);
    pos += n;
    reply[pos++] = ',';
    mbedtls_base64_encode((unsigned char *)reply + pos, sizeof(reply) - pos, &n,
                          (const unsigned char *)B, (size_t)len_B);
    pos += n;
    reply[pos] = 0;

    /* K_pake = first 32 bytes of the 64-byte SRP session key. Held from now
     * but only usable once state reaches LS_PAKE_OK. */
    memcpy(s->key, K, sizeof(s->key));
    s->srp   = hd;
    s->state = LS_PAKE_WAIT_PROOF;

    security_send_line(s->sock, reply);
    CY_LOGI(TCP_SERVER_DB, "pake: sock %d A(%d) -> B(%d), awaiting proof", s->sock, len_A, len_B);
}

/* PAKE3:<b64 M1> -> PAKE4:<b64 M2> */
static void handle_pake3(local_session_t *s, const char *line, int len){

    if(s->state != LS_PAKE_WAIT_PROOF || s->srp == NULL){
        send_err(s->sock, "SEQ");
        return;
    }

    uint8_t M1[SRP_PROOF_LEN];
    int len_M1 = decode_payload(line, len, PAKE3_PREFIX, M1, sizeof(M1));
    if(len_M1 != SRP_PROOF_LEN){
        /* Malformed, not a wrong password: no strike, but the attempt is over. */
        abort_pake(s);
        send_err(s->sock, "BADFMT");
        return;
    }

    uint8_t M2[SRP_PROOF_LEN];
    esp_err_t err = esp_srp_exchange_proofs((esp_srp_handle_t *)s->srp,
                                            DEVICE_PW_SRP_IDENTITY, strlen(DEVICE_PW_SRP_IDENTITY),
                                            (char *)M1, (char *)M2);
    if(err != ESP_OK){
        device_password_note_failure(s->peer_ip);
        abort_pake(s);
        send_err(s->sock, "BADPW");
        return;
    }

    /* Proof good: the SRP context has done its job; K_pake stays in s->key. */
    esp_srp_free((esp_srp_handle_t *)s->srp);
    s->srp = NULL;
    device_password_note_success(s->peer_ip);

    char reply[sizeof("PAKE4:") + B64_MAX(SRP_PROOF_LEN)];
    size_t n = 0;
    memcpy(reply, "PAKE4:", 6);
    mbedtls_base64_encode((unsigned char *)reply + 6, sizeof(reply) - 6, &n, M2, sizeof(M2));
    reply[6 + n] = 0;
    /* PAKE4 goes out in the clear; everything after it is encrypted under
     * K_pake, so the state moves only once the reply is on the wire. */
    security_send_line(s->sock, reply);
    s->tx_ctr = s->rx_ctr = 0;
    s->state = LS_PAKE_OK;
    CY_LOGI(TCP_SERVER_DB, "pake: sock %d proof ok", s->sock);
}

bool pake_handle_line(char *line, int len, int sock){

    bool is1 = (len >= (int)strlen(PAKE1_PREFIX) && memcmp(line, PAKE1_PREFIX, strlen(PAKE1_PREFIX)) == 0);
    bool is3 = (len >= (int)strlen(PAKE3_PREFIX) && memcmp(line, PAKE3_PREFIX, strlen(PAKE3_PREFIX)) == 0);
    if(!is1 && !is3) return false;

    local_session_t *s = local_session_find(sock);
    if(s == NULL){
        send_err(sock, "NOSESSION");
        return true;
    }
    if(is1) handle_pake1(s, line, len);
    else    handle_pake3(s, line, len);
    return true;
}
