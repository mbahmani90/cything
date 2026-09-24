#include "owner_commands.h"

#include <stdio.h>
#include <string.h>
#include "mbedtls/base64.h"

#include "common/cy_log.h"
#include "security/local_session.h"
#include "security/paired_list.h"
#include "security/device_password.h"
#include "security/pake_handler.h"     /* security_send_line */
#include "security/pairing_events.h"
#include "ble/discovery_mode.h"
#include "ble/ble_beacon.h"
#include "udp_socket/udp_server.h"

#define B64_MAX(n)  ((((n) + 2) / 3) * 4 + 1)

static bool has_prefix(const char *line, int len, const char *prefix){
    int n = strlen(prefix);
    return len >= n && memcmp(line, prefix, n) == 0;
}

static bool is_exact(const char *line, int len, const char *cmd){
    return len == (int)strlen(cmd) && memcmp(line, cmd, len) == 0;
}

static void send_err(int sock, const char *code){
    char buf[32];
    snprintf(buf, sizeof(buf), "ERR:%s", code);
    security_send_line(sock, buf);
}

/* Owner, authenticated, and the command arrived encrypted. */
static bool is_owner_session(const local_session_t *s){
    if(s == NULL || !s->rx_encrypted || s->role != LS_ROLE_OWNER) return false;
    return s->state == LS_AUTH_OK || (s->state == LS_PAKE_OK && s->paired_index >= 0);
}

/* Any authenticated, encrypted session with a known paired-list entry — the
 * caller may act on its OWN entry (UNPAIR), whatever its role. */
static bool is_authed_session(const local_session_t *s){
    if(s == NULL || !s->rx_encrypted || s->paired_index < 0) return false;
    return s->state == LS_AUTH_OK || s->state == LS_PAKE_OK;
}

/* Decode one base64 field between `p` and `end` (or the next ','). Advances
 * *p past the ','. Returns the decoded length or -1. */
static int next_b64_field(const char **p, const char *end, uint8_t *out, size_t out_size, bool last){
    const char *comma = memchr(*p, ',', end - *p);
    const char *fend  = comma ? comma : end;
    if(last && comma != NULL) return -1;
    if(!last && comma == NULL) return -1;
    size_t n = 0;
    if(mbedtls_base64_decode(out, out_size, &n, (const unsigned char *)*p, (size_t)(fend - *p)) != 0) return -1;
    *p = fend + 1;
    return (int)n;
}

static bool to_cstr(const uint8_t *in, int n, char *out, size_t out_size){
    if(n <= 0 || (size_t)n >= out_size) return false;
    for(int i = 0 ; i < n ; i++){
        if(in[i] < 0x20 || in[i] == 0x7f) return false;
    }
    memcpy(out, in, (size_t)n);
    out[n] = 0;
    return true;
}

/* ---- LIST ------------------------------------------------------------- */

static void handle_list(local_session_t *s){
    int count = paired_list_count();
    for(int i = 0 ; i < count ; i++){
        paired_entry_t e;
        if(!paired_list_get(i, &e)) break;

        char line[sizeof("PAIRED:") + B64_MAX(PAIRED_SUB_MAX) + B64_MAX(PAIRED_INSTALL_MAX) +
                  B64_MAX(PAIRED_NAME_MAX) + 3 + 10 + 1 + 6];
        size_t pos = 0, n = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, "PAIRED:");
        mbedtls_base64_encode((unsigned char *)line + pos, sizeof(line) - pos, &n, (const unsigned char *)e.user_sub, strlen(e.user_sub));
        pos += n; line[pos++] = ',';
        mbedtls_base64_encode((unsigned char *)line + pos, sizeof(line) - pos, &n, (const unsigned char *)e.install_id, strlen(e.install_id));
        pos += n; line[pos++] = ',';
        mbedtls_base64_encode((unsigned char *)line + pos, sizeof(line) - pos, &n, (const unsigned char *)e.display_name, strlen(e.display_name));
        pos += n;
        snprintf(line + pos, sizeof(line) - pos, ",%lu,%s", (unsigned long)e.paired_at,
                 e.role == PAIRED_ROLE_OWNER ? "owner" : "user");
        memset(e.key, 0, sizeof(e.key));
        security_send_line(s->sock, line);
    }
    char tail[24];
    snprintf(tail, sizeof(tail), "PAIREND:%d", count);
    security_send_line(s->sock, tail);
}

/* ---- REVOKE ----------------------------------------------------------- */

static void handle_revoke(local_session_t *s, const char *line, int len){
    const char *p = line + strlen("REVOKE:");
    const char *end = line + len;
    uint8_t f_sub[PAIRED_SUB_MAX + 1], f_inst[PAIRED_INSTALL_MAX + 1];
    char user_sub[PAIRED_SUB_MAX + 1], install[PAIRED_INSTALL_MAX + 1];

    int n1 = next_b64_field(&p, end, f_sub,  sizeof(f_sub),  false);
    int n2 = next_b64_field(&p, end, f_inst, sizeof(f_inst), true);
    if(!to_cstr(f_sub, n1, user_sub, sizeof(user_sub)) || !to_cstr(f_inst, n2, install, sizeof(install))){
        send_err(s->sock, "BADFMT");
        return;
    }

    int index = paired_list_find(user_sub, install);
    paired_entry_t e;
    if(index < 0 || !paired_list_get(index, &e)){
        send_err(s->sock, "UNKNOWN");
        return;
    }
    if(index == s->paired_index){
        /* Never the entry this session runs on — the device would be left
         * with no one to manage it. "Delete from my phone" is UNPAIR. Other
         * owner-role entries (the same account's old installs) may go. */
        send_err(s->sock, "OWNER");
        return;
    }
    const char *role = e.role == PAIRED_ROLE_OWNER ? "owner" : "user";
    if(paired_list_remove(index) != ESP_OK){
        send_err(s->sock, "STORE");
        return;
    }
    local_session_on_paired_removed(index);
    pairing_events_push("unpaired", user_sub, install, role);
    security_send_line(s->sock, "ACK");
    CY_LOGI(TCP_SERVER_DB, "owner: revoked %s / %s (%s)", user_sub, install, role);
}

/* ---- PWSET ------------------------------------------------------------ */

/* Rotating the password revokes every non-owner phone: K_phone is otherwise
 * independent of the password, so without this a phone that paired under the
 * old password would keep access. Their entries are deleted (they must re-pair
 * with the new password); the owner account's own phones are kept. Each removed
 * phone gets an "unpaired" event so the backend drops its remote access too.
 * (RESET still wipes everyone, including the owner.) */
static void revoke_all_non_owner(void){
    int i = 0;
    while(i < paired_list_count()){
        paired_entry_t e;
        if(!paired_list_get(i, &e)) break;
        if(e.role != PAIRED_ROLE_OWNER){
            char sub[PAIRED_SUB_MAX + 1], inst[PAIRED_INSTALL_MAX + 1];
            snprintf(sub,  sizeof(sub),  "%s", e.user_sub);
            snprintf(inst, sizeof(inst), "%s", e.install_id);
            if(paired_list_remove(i) == ESP_OK){
                /* index unchanged: the tail shifted down into slot i. */
                local_session_on_paired_removed(i);
                pairing_events_push("unpaired", sub, inst, "user");
                CY_LOGI(TCP_SERVER_DB, "owner: pw change revoked %s / %s", sub, inst);
            }else{
                i++;   /* avoid a spin if a store fails */
            }
        }else{
            i++;
        }
    }
}

static void handle_pwset(local_session_t *s, const char *line, int len){
    const char *p = line + strlen("PWSET:");
    uint8_t pw[DEVICE_PW_MAX_LEN + 1];
    int n = next_b64_field(&p, line + len, pw, sizeof(pw), true);
    if(n < DEVICE_PW_MIN_LEN || n > DEVICE_PW_MAX_LEN){
        send_err(s->sock, "BADFMT");
        return;
    }
    esp_err_t err = device_password_set((const char *)pw, (size_t)n);
    memset(pw, 0, sizeof(pw));
    if(err != ESP_OK){
        send_err(s->sock, "STORE");
        return;
    }
    /* A change (owner session) revokes every other phone; bootstrap has none. */
    if(s->role == LS_ROLE_OWNER){
        revoke_all_non_owner();
    }
    security_send_line(s->sock, "ACK");
    CY_LOGI(TCP_SERVER_DB, "owner: password %s", s->role == LS_ROLE_OWNER ? "changed" : "set (bootstrap)");
}

/* ---- PWCLEAR (open to all) -------------------------------------------- */

/* Remove the device password so the device is open — anyone on the LAN can
 * connect without pairing (see access_policy: no password => nothing is
 * protected). Owner-only. Unlike RESET the paired list is kept, so the owner
 * stays owner and can re-protect it later (their K_phone still works; pairings
 * are simply moot while the device is open). */
static void handle_pwclear(local_session_t *s){
    esp_err_t err = device_password_clear();
    if(err != ESP_OK){
        send_err(s->sock, "STORE");
        return;
    }
    security_send_line(s->sock, "ACK");
    CY_LOGW(TCP_SERVER_DB, "owner: password cleared — device is OPEN to all");
}

/* ---- RESET ------------------------------------------------------------ */

static void handle_reset(local_session_t *s){
    /* Reply while the session still has its key, then pull the rug. */
    security_send_line(s->sock, "ACK");
    paired_list_clear();
    device_password_clear();
    local_session_drop_all_auth();
    pairing_events_push("reset", "", "", "");
    CY_LOGW(TCP_SERVER_DB, "owner: RESET — paired list and password wiped");
}

/* ---- DISCOVERYMODE ---------------------------------------------------- */

static void handle_discoverymode(local_session_t *s, const char *line, int len){
    const char *p = line + strlen("DISCOVERYMODE:");
    int mode_len = len - (int)strlen("DISCOVERYMODE:");

    /* Validate mode is a single character: "1", "2", or "3" */
    if(mode_len != 1 || (*p < '1' || *p > '3')){
        send_err(s->sock, "BADFMT");
        return;
    }

    uint8_t mode_val = (uint8_t)(*p - '0');
    discovery_mode_t mode = (discovery_mode_t)mode_val;

    /* Store mode to NVS and apply immediately */
    if(discovery_mode_set(mode) != ESP_OK){
        send_err(s->sock, "STORE");
        return;
    }

    ble_beacon_apply_discovery_mode();     /* stop / (re)start the beacon */
    udp_server_reconfigure();              /* join / leave the multicast group */
    security_send_line(s->sock, "ACK");
    CY_LOGI(TCP_SERVER_DB, "owner: discovery mode set to %c", *p);
}

/* "DISCOVERYMODE?" -> "DISCOVERYMODE:<1|2|3>" — what the device currently
 * applies, so the app's radio buttons reflect the device and not the phone. */
static void handle_discoverymode_get(local_session_t *s){
    char line[24];
    snprintf(line, sizeof(line), "DISCOVERYMODE:%d", (int)discovery_mode_get());
    security_send_line(s->sock, line);
}

/* ---- UNPAIR (self) ---------------------------------------------------- */

/* The caller removes its OWN entry from the paired list — "delete this
 * device from my phone". Any authenticated session, not just the owner. If
 * the owner unpairs while other phones remain, entry 0 is promoted to owner
 * so the device stays manageable. */
static void handle_unpair(local_session_t *s){
    paired_entry_t e;
    if(!paired_list_get(s->paired_index, &e)){
        send_err(s->sock, "UNKNOWN");
        return;
    }
    bool was_owner = (e.role == PAIRED_ROLE_OWNER);
    /* Reply while the session key is still valid. */
    security_send_line(s->sock, "ACK");

    int index = s->paired_index;
    if(paired_list_remove(index) == ESP_OK){
        local_session_on_paired_removed(index);   /* de-auths this session too */
        if(was_owner) paired_list_promote_owner();
        pairing_events_push("unpaired", e.user_sub, e.install_id,
                            was_owner ? "owner" : "user");
        CY_LOGI(TCP_SERVER_DB, "unpair: %s / %s removed self", e.user_sub, e.install_id);
    }
}

/* ---- dispatch --------------------------------------------------------- */

bool owner_commands_handle_line(char *line, int len, int sock){

    bool is_list           = is_exact(line, len, "LIST");
    bool is_revoke         = has_prefix(line, len, "REVOKE:");
    bool is_pwset          = has_prefix(line, len, "PWSET:");
    bool is_pwclear        = is_exact(line, len, "PWCLEAR");
    bool is_reset          = is_exact(line, len, "RESET");
    bool is_discoverymode  = has_prefix(line, len, "DISCOVERYMODE:");
    bool is_discoverymode_q = is_exact(line, len, "DISCOVERYMODE?");
    bool is_unpair         = is_exact(line, len, "UNPAIR");
    if(!is_list && !is_revoke && !is_pwset && !is_pwclear && !is_reset && !is_discoverymode && !is_discoverymode_q && !is_unpair) return false;

    local_session_t *s = local_session_find(sock);
    if(s == NULL){
        send_err(sock, "NOSESSION");
        return true;
    }

    /* Bootstrap: no password yet, PWSET from anyone. Remember it on this
     * session: it borrowed the open state to set a password, and may need
     * to give it back with PWCLEAR (below) once it has enrolled, even if
     * ENROLL does not make it the owner. */
    if(is_pwset && !device_password_is_set()){
        s->bootstrap_claim = true;
        handle_pwset(s, line, len);
        return true;
    }

    /* UNPAIR acts on the caller's own entry — any authenticated session. */
    if(is_unpair){
        if(!is_authed_session(s)) send_err(sock, "AUTH");
        else                      handle_unpair(s);
        return true;
    }

    /* PWCLEAR from a session that bootstrapped its own password on this same
     * connection and has since enrolled: let it close the open window it
     * opened for itself, even without the OWNER role (paired_list may already
     * have a different owner_sub). One-shot — this grants nothing else
     * owner-gated, and a later PWCLEAR on this session needs real ownership. */
    if(is_pwclear && s->bootstrap_claim && is_authed_session(s) && !is_owner_session(s)){
        s->bootstrap_claim = false;
        handle_pwclear(s);
        return true;
    }

    if(!is_owner_session(s)){
        send_err(sock, "AUTH");
        return true;
    }

    if(is_list)              handle_list(s);
    else if(is_revoke)       handle_revoke(s, line, len);
    else if(is_pwset)        handle_pwset(s, line, len);
    else if(is_pwclear)      handle_pwclear(s);
    else if(is_discoverymode) handle_discoverymode(s, line, len);
    else if(is_discoverymode_q) handle_discoverymode_get(s);
    else                     handle_reset(s);
    return true;
}
