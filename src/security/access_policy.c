#include "access_policy.h"

#include <string.h>
#include "lwip/sockets.h"

#include "CyThingEsp32.h"
#include "common/cy_log.h"
#include "device_config/device_config.h"
#include "security/local_session.h"
#include "security/device_password.h"
#include "device_config/cy_config.h"

#ifndef LOCAL_AUTH_ENFORCE
#define LOCAL_AUTH_ENFORCE 0
#endif

static bool has_prefix(const char *line, int len, const char *prefix){
    int n = strlen(prefix);
    return len >= n && memcmp(line, prefix, n) == 0;
}

/* Fallback for app_command_is_public(); overridden by the application. */
__attribute__((weak)) bool app_command_is_public(const char *line, int len){
    (void)line; (void)len;
    return false;
}

static bool is_handshake(const char *line, int len){
    return has_prefix(line, len, "PAKE1:") || has_prefix(line, len, "PAKE3:") ||
           has_prefix(line, len, "AUTH1:") || has_prefix(line, len, "AUTH3:") ||
           has_prefix(line, len, "ENROLL:");
}

static bool is_public(const char *line, int len){
    if(has_prefix(line, len, cy_scan_command)) return true;
    /* No password yet: nothing can be protected, and PWSET: is how the
     * first password gets in over TCP (BLE provisioning is the other way). */
    if(!device_password_is_set()) return true;
    return app_command_is_public(line, len);
}

bool access_policy_sock_authenticated(int sock){
    local_session_t *s = local_session_find(sock);
    if(s == NULL) return false;
    return s->state == LS_AUTH_OK || (s->state == LS_PAKE_OK && s->paired_index >= 0);
}

#if LOCAL_AUTH_ENFORCE == 1
static void send_plain(int sock, const char *text){
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%s\n", text);
    send(sock, buf, n, 0);
}
#endif

bool access_policy_allow(const char *line, int len, int sock){

    if(is_handshake(line, len) || is_public(line, len)){
        return true;
    }

    local_session_t *s = local_session_find(sock);
    bool authenticated = access_policy_sock_authenticated(sock);
    bool encrypted     = (s != NULL && s->rx_encrypted);
    if(authenticated && encrypted){
        return true;
    }

#if LOCAL_AUTH_ENFORCE == 1
    if(!authenticated){
        CY_LOGW(TCP_SERVER_DB, "policy: sock %d not authenticated, refused: %.*s", sock, len > 24 ? 24 : len, line);
        send_plain(sock, "ERR:AUTH");
    }else{
        /* Authenticated socket, but the line came in the clear: anyone on
         * the LAN could have injected it, so it does not count. */
        CY_LOGW(TCP_SERVER_DB, "policy: sock %d protected line sent in plaintext, refused", sock);
        send_plain(sock, "ERR:ENC");
    }
    return false;
#else
    /* Not enforcing: accept, but say what would have happened so the
     * rollout can be checked from the serial log. */
    CY_LOGD(TCP_SERVER_DB, "policy: sock %d would refuse (%s): %.*s", sock,
            authenticated ? "plaintext" : "unpaired", len > 24 ? 24 : len, line);
    return true;
#endif
}
