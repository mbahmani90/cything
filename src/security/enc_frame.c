#include "enc_frame.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/version.h"

#include "common/cy_log.h"
#include "security/local_session.h"

/* AES-256-GCM through whichever API this mbedTLS exposes: 4.x (ESP-IDF 6.0)
 * only offers the PSA AEAD interface, 3.x (ESP-IDF 5.5, the Arduino builds)
 * the legacy mbedtls_gcm_* one. Same convention as aws/provisioning.c.
 * aead(): encrypt in=plaintext -> out=ciphertext‖tag;
 *         decrypt in=ciphertext‖tag -> out=plaintext. Returns 0 on success. */
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
static int aead(bool encrypt, const uint8_t key[32], const uint8_t nonce[ENC_FRAME_NONCE_LEN],
                const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len){
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_key_id_t id = 0;
    psa_status_t st = psa_import_key(&attr, key, 32, &id);
    if(st != PSA_SUCCESS) return (int)st;
    st = encrypt
        ? psa_aead_encrypt(id, PSA_ALG_GCM, nonce, ENC_FRAME_NONCE_LEN, NULL, 0, in, in_len, out, out_size, out_len)
        : psa_aead_decrypt(id, PSA_ALG_GCM, nonce, ENC_FRAME_NONCE_LEN, NULL, 0, in, in_len, out, out_size, out_len);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS) ? 0 : (int)st;
}
#else
#include "mbedtls/gcm.h"
static int aead(bool encrypt, const uint8_t key[32], const uint8_t nonce[ENC_FRAME_NONCE_LEN],
                const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len){
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if(rc == 0){
        if(encrypt){
            if(out_size < in_len + ENC_FRAME_TAG_LEN) rc = -1;
            else{
                rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, in_len, nonce, ENC_FRAME_NONCE_LEN,
                                               NULL, 0, in, out, ENC_FRAME_TAG_LEN, out + in_len);
                *out_len = in_len + ENC_FRAME_TAG_LEN;
            }
        }else{
            size_t plain_len = in_len - ENC_FRAME_TAG_LEN;
            if(in_len < ENC_FRAME_TAG_LEN || out_size < plain_len) rc = -1;
            else{
                rc = mbedtls_gcm_auth_decrypt(&gcm, plain_len, nonce, ENC_FRAME_NONCE_LEN, NULL, 0,
                                              in + plain_len, ENC_FRAME_TAG_LEN, in, out);
                *out_len = plain_len;
            }
        }
    }
    mbedtls_gcm_free(&gcm);
    return rc;
}
#endif

#define ENC_PREFIX      "ENC:"
#define ENC_PREFIX_LEN  4
#define B64_MAX(n)      ((((n) + 2) / 3) * 4 + 1)

/* One lock for every encrypted send: the recv task (handshake replies) and
 * tcp_response_send_task (FIFO lines) both send to the same socket, and
 * tx_ctr must be bumped and used atomically. The critical section is one
 * GCM call plus a send(). */
static SemaphoreHandle_t s_tx_mutex = NULL;

static bool session_keyed(const local_session_t *s){
    return s != NULL && (s->state == LS_PAKE_OK || s->state == LS_AUTH_OK);
}

static void put_u32(uint8_t *p, uint32_t v){
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put_u64(uint8_t *p, uint64_t v){
    put_u32(p, (uint32_t)(v >> 32)); put_u32(p + 4, (uint32_t)v);
}
static uint32_t get_u32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t get_u64(const uint8_t *p){
    return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

void enc_frame_init(void){
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex != NULL);
}

/* ---- receive ---------------------------------------------------------- */

static void send_plain_err(int sock, const char *text){
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    send(sock, buf, n, 0);
}

bool enc_frame_unwrap(char *line, int *len, int sock, size_t capacity){

    local_session_t *s = local_session_find(sock);
    bool is_enc = (*len > ENC_PREFIX_LEN && memcmp(line, ENC_PREFIX, ENC_PREFIX_LEN) == 0);

    if(!is_enc){
        if(s) s->rx_encrypted = false;
        return true;
    }
    if(!session_keyed(s)){
        send_plain_err(sock, "ERR:ENC");
        return false;
    }

    /* Split "<b64 nonce>,<b64 ct>" */
    const char *p     = line + ENC_PREFIX_LEN;
    const char *end   = line + *len;
    const char *comma = memchr(p, ',', end - p);
    if(comma == NULL){
        send_plain_err(sock, "ERR:ENC");
        return false;
    }

    uint8_t nonce[ENC_FRAME_NONCE_LEN + 1];
    size_t  nonce_len = 0;
    if(mbedtls_base64_decode(nonce, sizeof(nonce), &nonce_len, (const unsigned char *)p, comma - p) != 0 ||
       nonce_len != ENC_FRAME_NONCE_LEN){
        send_plain_err(sock, "ERR:ENC");
        return false;
    }

    /* The ciphertext decodes over the front of the frame text: base64 is
     * longer than what it encodes, so the decoder only ever reads ahead of
     * where it writes. */
    uint8_t *ct = (uint8_t *)line;
    size_t   ct_len = 0;
    if(mbedtls_base64_decode(ct, capacity, &ct_len, (const unsigned char *)comma + 1, end - (comma + 1)) != 0 ||
       ct_len < ENC_FRAME_TAG_LEN){
        send_plain_err(sock, "ERR:ENC");
        return false;
    }
    size_t plain_len = ct_len - ENC_FRAME_TAG_LEN;

    /* Direction + replay check before the (slower) decrypt. */
    uint64_t ctr = get_u64(nonce + 4);
    if(get_u32(nonce) != ENC_FRAME_DIR_TO_DEV || ctr <= s->rx_ctr){
        CY_LOGW(TCP_SERVER_DB, "enc: sock %d nonce rejected (dir %08x ctr %llu, last %llu)",
                sock, (unsigned)get_u32(nonce), (unsigned long long)ctr, (unsigned long long)s->rx_ctr);
        send_plain_err(sock, "ERR:ENC");
        return false;
    }

    /* Decrypt into a stack copy: in place is unsafe here (plaintext is
     * written where the tail of the ciphertext still has to be read). */
    uint8_t plain[ENC_FRAME_PLAIN_MAX];
    size_t  got = 0;
    if(plain_len > sizeof(plain)){
        send_plain_err(sock, "ERR:ENC");
        return false;
    }
    int rc = aead(false, s->key, nonce, ct, ct_len, plain, sizeof(plain), &got);
    if(rc != 0 || got != plain_len){
        CY_LOGW(TCP_SERVER_DB, "enc: sock %d frame failed authentication", sock);
        send_plain_err(sock, "ERR:ENC");
        return false;
    }

    s->rx_ctr = ctr;
    s->rx_encrypted = true;
    memcpy(line, plain, plain_len);
    line[plain_len]     = '\n';
    line[plain_len + 1] = 0;
    *len = (int)plain_len;
    return true;
}

/* ---- send ------------------------------------------------------------- */

void enc_frame_send(int sock, const char *payload, size_t payload_len){

    local_session_t *s = local_session_find(sock);
    if(!session_keyed(s)){
        send(sock, payload, payload_len, 0);
        return;
    }

    /* Plaintext is the line without its '\n'. */
    size_t plain_len = payload_len;
    while(plain_len > 0 && (payload[plain_len - 1] == '\n' || payload[plain_len - 1] == '\r')) plain_len--;
    if(plain_len > ENC_FRAME_PLAIN_MAX){
        CY_LOGW(TCP_SERVER_DB, "enc: sock %d line too long to encrypt (%u), dropped", sock, (unsigned)plain_len);
        return;
    }

    uint8_t ct[ENC_FRAME_PLAIN_MAX + ENC_FRAME_TAG_LEN];
    uint8_t nonce[ENC_FRAME_NONCE_LEN];
    char    frame[ENC_PREFIX_LEN + B64_MAX(ENC_FRAME_NONCE_LEN) + 1 + B64_MAX(sizeof(ct)) + 1];

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    /* Re-check under the lock: the session may have been released. */
    if(!session_keyed(s) || s->sock != sock){
        xSemaphoreGive(s_tx_mutex);
        return;
    }
    s->tx_ctr++;
    put_u32(nonce, ENC_FRAME_DIR_TO_APP);
    put_u64(nonce + 4, s->tx_ctr);

    size_t ct_len = 0;
    int rc = aead(true, s->key, nonce, (const uint8_t *)payload, plain_len, ct, sizeof(ct), &ct_len);

    if(rc == 0){
        size_t pos = ENC_PREFIX_LEN, n = 0;
        memcpy(frame, ENC_PREFIX, ENC_PREFIX_LEN);
        mbedtls_base64_encode((unsigned char *)frame + pos, sizeof(frame) - pos, &n, nonce, sizeof(nonce));
        pos += n;
        frame[pos++] = ',';
        mbedtls_base64_encode((unsigned char *)frame + pos, sizeof(frame) - pos, &n, ct, ct_len);
        pos += n;
        frame[pos++] = '\n';
        send(sock, frame, pos, 0);
    }else{
        CY_LOGE(TCP_SERVER_DB, "enc: sock %d encrypt failed (%d)", sock, rc);
    }

    xSemaphoreGive(s_tx_mutex);
}
