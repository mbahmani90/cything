#include "local_auth.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "esp_random.h"

#include "common/cy_log.h"
#include "security/local_session.h"
#include "security/paired_list.h"
#include "security/pake_handler.h"     /* security_send_line */
#include "security/pairing_events.h"

#define ENROLL_PREFIX  "ENROLL:"
#define AUTH1_PREFIX   "AUTH1:"
#define AUTH3_PREFIX   "AUTH3:"

#define B64_MAX(n)     ((((n) + 2) / 3) * 4 + 1)

/* ---- crypto helpers --------------------------------------------------- */

void local_auth_hmac(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len, uint8_t out[LOCAL_AUTH_MAC_LEN]){
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, key, key_len, msg, msg_len, out);
}

/* RFC 5869 with L = 32: PRK = HMAC(salt, ikm); OKM = HMAC(PRK, info ‖ 0x01). */
void local_auth_hkdf32(const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *salt, size_t salt_len,
                       const char *info, uint8_t out[32]){
    uint8_t prk[LOCAL_AUTH_MAC_LEN];
    local_auth_hmac(salt, salt_len, ikm, ikm_len, prk);

    uint8_t block[64];
    size_t info_len = strlen(info);
    if(info_len > sizeof(block) - 1) info_len = sizeof(block) - 1;
    memcpy(block, info, info_len);
    block[info_len] = 0x01;
    local_auth_hmac(prk, sizeof(prk), block, info_len + 1, out);
    memset(prk, 0, sizeof(prk));
}

bool local_auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n){
    uint8_t d = 0;
    for(size_t i = 0 ; i < n ; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* HMAC(key, label ‖ first ‖ second) — the shape of both AUTH proofs. */
static void proof(const uint8_t key[32], const char *label,
                  const uint8_t *first, const uint8_t *second, uint8_t out[LOCAL_AUTH_MAC_LEN]){
    uint8_t msg[3 + 2 * LOCAL_AUTH_NONCE_LEN];
    memcpy(msg, label, 3);
    memcpy(msg + 3, first,  LOCAL_AUTH_NONCE_LEN);
    memcpy(msg + 3 + LOCAL_AUTH_NONCE_LEN, second, LOCAL_AUTH_NONCE_LEN);
    local_auth_hmac(key, 32, msg, sizeof(msg), out);
}

/* ---- line helpers ----------------------------------------------------- */

static void send_err(int sock, const char *code){
    char buf[32];
    snprintf(buf, sizeof(buf), "ERR:%s", code);
    security_send_line(sock, buf);
}

/* Split the payload after `prefix` on ',' into up to `max` base64 fields
 * and decode each into its own buffer. Every field must be present and
 * decode cleanly; `lens[i]` receives the decoded length. Returns false on
 * any format error. */
static bool decode_fields(const char *line, int len, const char *prefix,
                          uint8_t **bufs, const size_t *sizes, size_t *lens, int max){
    int plen = strlen(prefix);
    if(len <= plen) return false;
    const char *p   = line + plen;
    const char *end = line + len;
    for(int i = 0 ; i < max ; i++){
        const char *comma = memchr(p, ',', end - p);
        const char *fend  = comma ? comma : end;
        if(i < max - 1 && comma == NULL) return false;   /* too few fields */
        if(i == max - 1 && comma != NULL) return false;  /* too many */
        if(mbedtls_base64_decode(bufs[i], sizes[i], &lens[i],
                                 (const unsigned char *)p, (size_t)(fend - p)) != 0){
            return false;
        }
        p = fend + 1;
    }
    return true;
}

/* Decoded text field -> NUL-terminated C string, refusing embedded NULs
 * and control characters. */
static bool to_cstr(const uint8_t *in, size_t n, char *out, size_t out_size){
    if(n == 0 || n >= out_size) return false;
    for(size_t i = 0 ; i < n ; i++){
        if(in[i] < 0x20 || in[i] == 0x7f) return false;
    }
    memcpy(out, in, n);
    out[n] = 0;
    return true;
}

static uint32_t now_unix(void){
    time_t t = time(NULL);
    return (t > 1600000000) ? (uint32_t)t : 0;   /* 0 until the clock is set */
}

/* ---- ENROLL ----------------------------------------------------------- */

static void handle_enroll(local_session_t *s, const char *line, int len){

    /* Only inside the PAKE-encrypted session: K_phone is about to be sent. */
    if(s->state != LS_PAKE_OK || !s->rx_encrypted){
        send_err(s->sock, "SEQ");
        return;
    }

    uint8_t f_sub[PAIRED_SUB_MAX + 1], f_name[PAIRED_NAME_MAX + 1], f_inst[PAIRED_INSTALL_MAX + 1];
    uint8_t *bufs[3]  = { f_sub, f_name, f_inst };
    size_t   sizes[3] = { sizeof(f_sub), sizeof(f_name), sizeof(f_inst) };
    size_t   lens[3];
    char user_sub[PAIRED_SUB_MAX + 1], name[PAIRED_NAME_MAX + 1], install[PAIRED_INSTALL_MAX + 1];

    if(!decode_fields(line, len, ENROLL_PREFIX, bufs, sizes, lens, 3) ||
       !to_cstr(f_sub,  lens[0], user_sub, sizeof(user_sub)) ||
       !to_cstr(f_name, lens[1], name,     sizeof(name))     ||
       !to_cstr(f_inst, lens[2], install,  sizeof(install))){
        send_err(s->sock, "BADFMT");
        return;
    }

    uint8_t k_phone[PAIRED_KEY_LEN];
    esp_fill_random(k_phone, sizeof(k_phone));

    paired_role_t role;
    int index = paired_list_add(user_sub, install, name, k_phone, now_unix(), &role);
    if(index < 0){
        memset(k_phone, 0, sizeof(k_phone));
        send_err(s->sock, "FULL");
        return;
    }
    s->paired_index = index;
    s->role = (role == PAIRED_ROLE_OWNER) ? LS_ROLE_OWNER : LS_ROLE_USER;

    char reply[sizeof("ENROLLED:") + B64_MAX(PAIRED_KEY_LEN) + sizeof(",owner")];
    size_t n = 0;
    int pos = snprintf(reply, sizeof(reply), "ENROLLED:");
    mbedtls_base64_encode((unsigned char *)reply + pos, sizeof(reply) - pos, &n, k_phone, sizeof(k_phone));
    pos += n;
    snprintf(reply + pos, sizeof(reply) - pos, ",%s", role == PAIRED_ROLE_OWNER ? "owner" : "user");
    memset(k_phone, 0, sizeof(k_phone));

    security_send_line(s->sock, reply);
    pairing_events_push("paired", user_sub, install, role == PAIRED_ROLE_OWNER ? "owner" : "user");
    CY_LOGI(TCP_SERVER_DB, "local_auth: sock %d enrolled %s (%s) as %s", s->sock, user_sub, name,
            role == PAIRED_ROLE_OWNER ? "owner" : "user");
}

/* ---- AUTH1 / AUTH3 ---------------------------------------------------- */

static void handle_auth1(local_session_t *s, const char *line, int len){

    /* A fresh AUTH1 abandons any in-progress or established handshake and
     * starts over (mirror of PAKE1); see local_session_reset_handshake(). */
    local_session_reset_handshake(s);

    uint8_t f_sub[PAIRED_SUB_MAX + 1], f_inst[PAIRED_INSTALL_MAX + 1], f_np[LOCAL_AUTH_NONCE_LEN + 1];
    uint8_t *bufs[3]  = { f_sub, f_inst, f_np };
    size_t   sizes[3] = { sizeof(f_sub), sizeof(f_inst), sizeof(f_np) };
    size_t   lens[3];
    char user_sub[PAIRED_SUB_MAX + 1], install[PAIRED_INSTALL_MAX + 1];

    if(!decode_fields(line, len, AUTH1_PREFIX, bufs, sizes, lens, 3) ||
       !to_cstr(f_sub,  lens[0], user_sub, sizeof(user_sub)) ||
       !to_cstr(f_inst, lens[1], install,  sizeof(install))  ||
       lens[2] != LOCAL_AUTH_NONCE_LEN){
        send_err(s->sock, "BADFMT");
        return;
    }

    paired_entry_t entry;
    int index = paired_list_find(user_sub, install);
    if(index < 0 || !paired_list_get(index, &entry)){
        send_err(s->sock, "UNKNOWN");
        return;
    }

    memcpy(s->nonce_phone, f_np, LOCAL_AUTH_NONCE_LEN);
    esp_fill_random(s->nonce_dev, LOCAL_AUTH_NONCE_LEN);
    memcpy(s->key, entry.key, sizeof(s->key));      /* K_phone until AUTHOK */
    memset(&entry, 0, sizeof(entry));
    s->paired_index = index;
    s->state = LS_AUTH_WAIT_PROOF;

    uint8_t mac[LOCAL_AUTH_MAC_LEN];
    proof(s->key, "dev", s->nonce_phone, s->nonce_dev, mac);

    char reply[sizeof("AUTH2:") + B64_MAX(LOCAL_AUTH_NONCE_LEN) + 1 + B64_MAX(LOCAL_AUTH_MAC_LEN)];
    size_t n = 0;
    int pos = snprintf(reply, sizeof(reply), "AUTH2:");
    mbedtls_base64_encode((unsigned char *)reply + pos, sizeof(reply) - pos, &n, s->nonce_dev, LOCAL_AUTH_NONCE_LEN);
    pos += n;
    reply[pos++] = ',';
    mbedtls_base64_encode((unsigned char *)reply + pos, sizeof(reply) - pos, &n, mac, sizeof(mac));
    reply[pos + n] = 0;

    security_send_line(s->sock, reply);
}

static void handle_auth3(local_session_t *s, const char *line, int len){

    if(s->state != LS_AUTH_WAIT_PROOF){
        send_err(s->sock, "SEQ");
        return;
    }

    uint8_t got[LOCAL_AUTH_MAC_LEN + 1];
    uint8_t *bufs[1]  = { got };
    size_t   sizes[1] = { sizeof(got) };
    size_t   lens[1];
    if(!decode_fields(line, len, AUTH3_PREFIX, bufs, sizes, lens, 1) || lens[0] != LOCAL_AUTH_MAC_LEN){
        send_err(s->sock, "BADFMT");
        return;
    }

    uint8_t expected[LOCAL_AUTH_MAC_LEN];
    proof(s->key, "phn", s->nonce_dev, s->nonce_phone, expected);

    if(!local_auth_ct_equal(got, expected, LOCAL_AUTH_MAC_LEN)){
        memset(s->key, 0, sizeof(s->key));
        s->paired_index = -1;
        s->state = LS_NEW;
        send_err(s->sock, "BADAUTH");
        CY_LOGW(TCP_SERVER_DB, "local_auth: sock %d bad phone proof", s->sock);
        return;
    }

    /* K_session replaces K_phone in the slot; the paired entry keeps K_phone. */
    uint8_t salt[2 * LOCAL_AUTH_NONCE_LEN];
    memcpy(salt, s->nonce_phone, LOCAL_AUTH_NONCE_LEN);
    memcpy(salt + LOCAL_AUTH_NONCE_LEN, s->nonce_dev, LOCAL_AUTH_NONCE_LEN);
    uint8_t k_session[32];
    local_auth_hkdf32(s->key, sizeof(s->key), salt, sizeof(salt), LOCAL_AUTH_HKDF_INFO, k_session);
    memcpy(s->key, k_session, sizeof(s->key));
    memset(k_session, 0, sizeof(k_session));

    paired_entry_t entry;
    s->role = LS_ROLE_USER;
    if(paired_list_get(s->paired_index, &entry)){
        if(entry.role == PAIRED_ROLE_OWNER) s->role = LS_ROLE_OWNER;
        memset(&entry, 0, sizeof(entry));
    }
    /* AUTHOK is the last plaintext line; the state moves once it is sent.
     * It carries the role (like ENROLLED) so a reconnecting phone learns it
     * without a LIST — which only an owner may send. */
    security_send_line(s->sock, s->role == LS_ROLE_OWNER ? "AUTHOK:owner" : "AUTHOK:user");
    s->tx_ctr = s->rx_ctr = 0;
    s->state = LS_AUTH_OK;
    CY_LOGI(TCP_SERVER_DB, "local_auth: sock %d authenticated (entry #%d, %s)", s->sock, s->paired_index,
            s->role == LS_ROLE_OWNER ? "owner" : "user");
}

/* ---- dispatch --------------------------------------------------------- */

bool local_auth_handle_line(char *line, int len, int sock){

    #define HAS(p) (len >= (int)strlen(p) && memcmp(line, p, strlen(p)) == 0)
    bool is_enroll = HAS(ENROLL_PREFIX);
    bool is_auth1  = HAS(AUTH1_PREFIX);
    bool is_auth3  = HAS(AUTH3_PREFIX);
    #undef HAS
    if(!is_enroll && !is_auth1 && !is_auth3) return false;

    local_session_t *s = local_session_find(sock);
    if(s == NULL){
        send_err(sock, "NOSESSION");
        return true;
    }
    if(is_enroll)     handle_enroll(s, line, len);
    else if(is_auth1) handle_auth1(s, line, len);
    else              handle_auth3(s, line, len);
    return true;
}
