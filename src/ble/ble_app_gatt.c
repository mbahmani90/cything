/*
 * Station-mode GATT server — see ble_app_gatt.h and doc/ble-gatt-server.md.
 *
 * The beacon (ble_beacon.c) owns the host, the advertising and the GAP
 * callback; this file owns the attribute table, the access callbacks, the
 * per-connection state and the access policy:
 *
 *   - the channel's services, built at runtime from cy_ble_services into
 *     NimBLE ble_gatt_svc_def / ble_gatt_chr_def arrays (heap, once);
 *   - the AUTH service (BLE_APP_AUTH_SVC_ID): an INFO characteristic (the
 *     GET_INFO CSV) and a line link where a phone runs the same PAKE1..4,
 *     ENROLL and AUTH1..3 lines as on TCP. Each connection is a
 *     pseudo-socket in the local_session pool (security/local_link_io.h),
 *     so pake_handler.c / local_auth.c / enc_frame.c serve it unchanged;
 *     the lines run on a short-lived worker task (SRP-3072 needs ~10 KB of
 *     stack and a second or two, too much for the host task);
 *   - LOCAL_AUTH_ENFORCE decides, as on TCP, whether a characteristic needs
 *     an authenticated phone behind the link (access_allowed()).
 *
 * Threads: access callbacks and GAP events run on the NimBLE host task;
 * cything_ble_notify() runs on whichever task calls it. `lock` guards the
 * connection table. It is never held across a NimBLE call: a notification
 * fires BLE_GAP_EVENT_NOTIFY_TX synchronously on the caller's task.
 */
#include "ble_app_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ble/ble_nimble.h"
#include "ble/ble_config.h"
#include "ble/ble_host.h"
#include "common/cy_log.h"
#include "device_config/device_config.h"
#include "common/task_config.h"
#include "security/access_policy.h"
#include "security/device_password.h"
#include "security/enc_frame.h"
#include "security/local_auth.h"
#include "security/local_link_io.h"
#include "security/local_session.h"
#include "security/pake_handler.h"
#include "udp_socket/udp_response_handler.h"
#include "udp_socket/udp_server.h"

#ifndef LOCAL_AUTH_ENFORCE
#define LOCAL_AUTH_ENFORCE 0
#endif

#ifdef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define MAX_CONN CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#else
#define MAX_CONN 3
#endif

/* ----------------------------------------------------- weak defaults --- */

/* cy_ble_services / cy_ble_service_count default to NULL / 0 in
 * device_config/cy_config.c, NOT here: GCC folds a weak const scalar it can
 * see the initializer of, so a default in this file would turn
 * ble_app_gatt_service_count() into "return 0" and the sketch's table would
 * never be read (seen with xtensa GCC 15). */

__attribute__((weak)) bool app_ble_write(const char *name, const uint8_t *data, int len){
    (void)data;
    CY_LOGW(BLE_DB, "gatt: write to \"%s\" (%d B) but no app_ble_write()", name, len);
    return false;
}

__attribute__((weak)) int app_ble_read(const char *name, uint8_t *out, int max){
    (void)name; (void)out; (void)max;
    return 0;
}

__attribute__((weak)) bool app_ble_is_public(const char *name){
    (void)name;
    return false;
}

/* --------------------------------------------------------------- table --- */

typedef struct {
    const cything_ble_chr_t *def;
    uint16_t                 val_handle;   /* filled by ble_gatts_add_svcs() */
} chr_rt_t;

static int                      svc_total;
static int                      chr_total;
static chr_rt_t                *chr_rt;      /* [chr_total], table order */
static ble_uuid_any_t          *uuids;       /* [svc_total + chr_total]  */
static struct ble_gatt_chr_def *chr_defs;    /* per service: its chrs + {0}; then auth + {0} */
static struct ble_gatt_svc_def *svc_defs;    /* svc_total + auth + {0}   */

static ble_uuid128_t auth_svc_uuid;
static ble_uuid128_t auth_chr_uuid;
static ble_uuid128_t info_chr_uuid;

static bool running = false;

/* -------------------------------------------------------- connections --- */

/* The line link on the auth characteristic (doc/ble-gatt-server.md, "Line
 * link"). An attribute holds at most 512 bytes and PAKE lines are longer,
 * so both directions move in chunks of up to LINK_CHUNK bytes:
 *   phone -> device: writes of up to LINK_CHUNK bytes, the line ending in '\n';
 *   device -> phone: a read returns the current chunk of the reply (empty
 *                    while the worker is still on it); a write of a lone
 *                    "\n" with no line in progress moves to the next chunk. */
#define LINK_CHUNK     500
#define LINK_LINE_MAX  1200            /* an ENC: frame of ENC_FRAME_PLAIN_MAX, base64 */
#define CONN_DRAINING  0xFFFE          /* disconnected, its worker still running */

typedef struct {
    uint16_t handle;                   /* BLE_HS_CONN_HANDLE_NONE: free slot */
    int      sock;                     /* pseudo-socket in the local_session pool, -1: none */
    bool     busy;                     /* a line is with the worker */
    char    *rx;                       /* [LINK_LINE_MAX + 2]: the line being written */
    int      rx_len;
    char    *tx;                       /* [LINK_LINE_MAX + 2]: the reply being read */
    int      tx_len;
    int      tx_head;                  /* start of the chunk a read returns */
    uint8_t *subs;                     /* bitmap, one bit per chr_rt entry */
} conn_t;

static SemaphoreHandle_t lock;
static conn_t            conns[MAX_CONN];
static int               subs_bytes;

static conn_t *conn_find(uint16_t handle){
    for(int i = 0 ; i < MAX_CONN ; i++){
        if(conns[i].handle == handle) return &conns[i];
    }
    return NULL;
}

/* Live or draining connection on pseudo-socket `sock`. */
static conn_t *conn_by_sock(int sock){
    for(int i = 0 ; i < MAX_CONN ; i++){
        if(conns[i].handle != BLE_HS_CONN_HANDLE_NONE && conns[i].sock == sock) return &conns[i];
    }
    return NULL;
}

/* Caller holds `lock`. Frees the link buffers, keeps the subscription bitmap. */
static void conn_wipe(conn_t *c){
    uint8_t *subs = c->subs;
    free(c->rx);
    memset(c, 0, sizeof(*c));
    c->handle = BLE_HS_CONN_HANDLE_NONE;
    c->sock = -1;
    c->subs = subs;
    if(subs != NULL) memset(subs, 0, subs_bytes);
}

static int chr_by_name(const char *name){
    for(int i = 0 ; i < chr_total ; i++){
        if(strcmp(chr_rt[i].def->name, name) == 0) return i;
    }
    return -1;
}

static int chr_by_handle(uint16_t val_handle){
    for(int i = 0 ; i < chr_total ; i++){
        if(chr_rt[i].val_handle == val_handle) return i;
    }
    return -1;
}

/* ------------------------------------------------------------- policy --- */

/* Caller holds `lock`. The phone behind `c` has paired (ENROLL) or
 * authenticated (AUTH) on this connection, exactly as access_policy counts
 * a TCP socket; REVOKE / RESET / PWSET drop that through local_session. Or
 * the device is open (no password: everything is public, as on TCP). */
static bool conn_authorized_locked(conn_t *c){
    if(!device_password_is_set()) return true;
    return c != NULL && c->sock >= 0 && access_policy_sock_authenticated(c->sock);
}

/* May the phone on `conn_handle` read / write / be notified on `idx`? */
static bool access_allowed(uint16_t conn_handle, int idx){
    const char *name = chr_rt[idx].def->name;
    if(app_ble_is_public(name)) return true;

    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = conn_authorized_locked(conn_find(conn_handle));
    xSemaphoreGive(lock);
    if(ok) return true;

#if LOCAL_AUTH_ENFORCE == 1
    CY_LOGW(BLE_DB, "gatt: conn %u not authenticated, refused \"%s\"", conn_handle, name);
    return false;
#else
    CY_LOGD(BLE_DB, "gatt: conn %u would refuse \"%s\" (unauthenticated)", conn_handle, name);
    return true;
#endif
}

/* ------------------------------------------------------------- access --- */

static uint8_t io_buf[CYTHING_BLE_VALUE_MAX + 1];   /* host task only */

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)attr_handle;
    int idx = (int)(intptr_t)arg;
    const cything_ble_chr_t *def = chr_rt[idx].def;

    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR){
        if(!access_allowed(conn_handle, idx)) return CYTHING_BLE_ATT_ERR_AUTH;
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if(len > CYTHING_BLE_VALUE_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if(ble_hs_mbuf_to_flat(ctxt->om, io_buf, CYTHING_BLE_VALUE_MAX, &len) != 0){
            return BLE_ATT_ERR_UNLIKELY;
        }
        io_buf[len] = 0;    /* convenience for text values; not counted in len */
        return app_ble_write(def->name, io_buf, len) ? 0 : CYTHING_BLE_ATT_ERR_REJECTED;
    }

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR){
        if(!access_allowed(conn_handle, idx)) return CYTHING_BLE_ATT_ERR_AUTH;
        int n = app_ble_read(def->name, io_buf, CYTHING_BLE_VALUE_MAX);
        if(n < 0) n = 0;
        if(n > CYTHING_BLE_VALUE_MAX) n = CYTHING_BLE_VALUE_MAX;
        return os_mbuf_append(ctxt->om, io_buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------------------------------------------------------- line link --- */

/* local_link_send() for a BLE pseudo-socket: append to the reply the phone
 * reads. Runs on the worker task. */
void ble_app_gatt_link_send(int sock, const void *data, size_t len){
    xSemaphoreTake(lock, portMAX_DELAY);
    conn_t *c = conn_by_sock(sock);
    if(c != NULL && c->tx != NULL){
        if(c->tx_len + (int)len <= LINK_LINE_MAX + 2){
            memcpy(c->tx + c->tx_len, data, len);
            c->tx_len += (int)len;
        }else{
            CY_LOGW(BLE_DB, "gatt: link reply too long (%u), dropped", (unsigned)len);
        }
    }
    xSemaphoreGive(lock);
}

typedef struct {
    conn_t *conn;
    int     sock;
    int     len;
    char    line[LINK_LINE_MAX + 2];   /* the line, '\n', NUL: what the TCP dispatch passes */
} link_job_t;

/* One line through the same handlers as tcp_dispatch_line(): ENC: unwrap,
 * then PAKE or ENROLL / AUTH. Nothing else runs over the BLE link. */
static void link_worker(void *arg){
    link_job_t *job = arg;
    int len = job->len;
    if(enc_frame_unwrap(job->line, &len, job->sock, sizeof(job->line))){
        if(!pake_handle_line(job->line, len, job->sock) && !local_auth_handle_line(job->line, len, job->sock)){
            security_send_line(job->sock, "ERR:BADFMT");
        }
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    conn_t *c = job->conn;
    c->busy = false;
    if(c->handle == CONN_DRAINING){              /* the phone left while we worked */
        local_session_release(c->sock);
        conn_wipe(c);
    }
    xSemaphoreGive(lock);
    free(job);
    vTaskDelete(NULL);
}

static int auth_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)attr_handle; (void)arg;
    static char chunk[LINK_CHUNK];               /* host task only */

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR){
        int n = 0;
        xSemaphoreTake(lock, portMAX_DELAY);
        conn_t *c = conn_find(conn_handle);
        if(c != NULL && c->tx != NULL){
            n = c->tx_len - c->tx_head;
            if(n > LINK_CHUNK) n = LINK_CHUNK;
            if(n < 0) n = 0;
            memcpy(chunk, c->tx + c->tx_head, n);
        }
        xSemaphoreGive(lock);
        return os_mbuf_append(ctxt->om, chunk, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if(ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR){
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if(len > LINK_CHUNK) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if(ble_hs_mbuf_to_flat(ctxt->om, chunk, LINK_CHUNK, &len) != 0) return BLE_ATT_ERR_UNLIKELY;

    int rc = 0;
    link_job_t *job = NULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    conn_t *c = conn_find(conn_handle);
    if(c == NULL || c->rx == NULL || c->sock < 0){
        rc = BLE_ATT_ERR_UNLIKELY;
    }else if(c->rx_len == 0 && len == 1 && chunk[0] == '\n'){
        /* "next chunk" */
        int n = c->tx_len - c->tx_head;
        c->tx_head += n > LINK_CHUNK ? LINK_CHUNK : n;
        if(c->tx_head >= c->tx_len) c->tx_head = c->tx_len = 0;
    }else if(c->busy){
        rc = CYTHING_BLE_ATT_ERR_BUSY;
    }else if(c->rx_len + len > LINK_LINE_MAX + 1){
        c->rx_len = 0;
        rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }else{
        memcpy(c->rx + c->rx_len, chunk, len);
        c->rx_len += len;
        if(c->rx[c->rx_len - 1] == '\n'){
            int n = c->rx_len - 1;
            while(n > 0 && c->rx[n - 1] == '\r') n--;
            c->rx_len = 0;
            c->tx_len = c->tx_head = 0;          /* a new request replaces the old reply */
            job = malloc(sizeof(*job));
            if(job != NULL && n > 0){
                job->conn = c;
                job->sock = c->sock;
                job->len  = n;
                memcpy(job->line, c->rx, n);
                job->line[n] = '\n';
                job->line[n + 1] = 0;
                c->busy = true;
                if(xTaskCreate(link_worker, "ble_link", BLE_LINK_TASK_STACK_SIZE, job,
                               BLE_LINK_TASK_PRIORITY, NULL) != pdPASS){
                    c->busy = false;
                    free(job);
                    job = NULL;
                    rc = BLE_ATT_ERR_INSUFFICIENT_RES;
                }
            }else{
                free(job);
                job = NULL;
            }
        }
    }
    xSemaphoreGive(lock);
    return rc;
}

/* --------------------------------------------------------------- info --- */

/* The GET_INFO CSV, as the UDP scan reply and the pairing service's Info
 * characteristic carry it: deviceId, caps (nopw / authreq / email), ... A
 * phone that found the device over BLE reads it to learn which device this
 * is (the key it holds is stored under that deviceId) and whether AUTH is
 * needed. Public, like GET_INFO on the LAN. */
static int info_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)conn_handle; (void)attr_handle; (void)arg;
    static char info[UDP_REPLY_MAX];   /* host task only */
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    int n = udp_get_info_response(info, sizeof(info));
    return os_mbuf_append(ctxt->om, info, n > 0 ? (size_t)n : 0) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ------------------------------------------------------------- notify --- */

int cything_ble_notify(const char *name, const uint8_t *data, int len){
    if(name == NULL || (data == NULL && len > 0) || len < 0 || len > CYTHING_BLE_VALUE_MAX){
        return CYTHING_BLE_ERR_ARG;
    }
    if(!running) return CYTHING_BLE_ERR_NOT_RUNNING;
    int idx = chr_by_name(name);
    if(idx < 0) return CYTHING_BLE_ERR_UNKNOWN_CHR;
    if(!(chr_rt[idx].def->flags & CYTHING_BLE_NOTIFY)) return CYTHING_BLE_ERR_NOT_NOTIFY;

    /* Snapshot the targets, then notify without the lock. */
    uint16_t targets[MAX_CONN];
    int n_targets = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for(int i = 0 ; i < MAX_CONN ; i++){
        conn_t *c = &conns[i];
        if(c->handle != BLE_HS_CONN_HANDLE_NONE && c->handle != CONN_DRAINING &&
           (c->subs[idx / 8] & (1u << (idx % 8)))){
            targets[n_targets++] = c->handle;
        }
    }
    xSemaphoreGive(lock);

    int sent = 0;
    bool no_mem = false;
    for(int i = 0 ; i < n_targets ; i++){
        if(!access_allowed(targets[i], idx)) continue;
        int room = (int)ble_att_mtu(targets[i]) - 3;
        int n = len;
        if(room > 0 && n > room){
            CY_LOGW(BLE_DB, "gatt: notify \"%s\" cut %d -> %d B (MTU)", name, len, room);
            n = room;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
        if(om == NULL){
            no_mem = true;
            continue;
        }
        int rc = ble_gatts_notify_custom(targets[i], chr_rt[idx].val_handle, om);   /* consumes om */
        if(rc == 0){
            sent++;
        }else if(rc == BLE_HS_ENOMEM){
            no_mem = true;
        }else{
            CY_LOGW(BLE_DB, "gatt: notify \"%s\" to conn %u failed, rc=%d", name, targets[i], rc);
        }
    }
    if(sent > 0) return sent;
    return no_mem ? CYTHING_BLE_ERR_NO_MEM : CYTHING_BLE_ERR_NO_SUBSCRIBER;
}

/* ---------------------------------------------------------------- GAP --- */

int ble_app_gatt_gap_event(struct ble_gap_event *event){
    if(!running) return 0;

    switch(event->type){
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0){
            xSemaphoreTake(lock, portMAX_DELAY);
            conn_t *c = conn_find(BLE_HS_CONN_HANDLE_NONE);
            if(c != NULL){
                conn_wipe(c);
                c->handle = event->connect.conn_handle;
                /* The line link: buffers and a session only while connected.
                 * Without them the channel's public characteristics still work. */
                c->rx = malloc(2 * (LINK_LINE_MAX + 2));
                if(c->rx != NULL){
                    c->tx = c->rx + LINK_LINE_MAX + 2;
                    int sock = LOCAL_LINK_BLE_SOCK_BASE + event->connect.conn_handle;
                    if(local_session_acquire(sock, LOCAL_LINK_BLE_PEER_IP) != NULL) c->sock = sock;
                }
            }
            xSemaphoreGive(lock);
            if(c == NULL){
                /* Cannot happen with MAX_CONN = the host's limit; refuse anyway. */
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }else{
                CY_LOGI(BLE_DB, "gatt: conn %u connected", event->connect.conn_handle);
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT: {
        xSemaphoreTake(lock, portMAX_DELAY);
        conn_t *c = conn_find(event->disconnect.conn.conn_handle);
        if(c != NULL){
            if(c->busy){
                c->handle = CONN_DRAINING;           /* the worker releases it */
            }else{
                if(c->sock >= 0) local_session_release(c->sock);
                conn_wipe(c);
            }
        }
        xSemaphoreGive(lock);
        CY_LOGI(BLE_DB, "gatt: conn %u disconnected, reason %d",
                event->disconnect.conn.conn_handle, event->disconnect.reason);
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = event->subscribe.attr_handle;
        bool on = event->subscribe.cur_notify;
        xSemaphoreTake(lock, portMAX_DELAY);
        conn_t *c = conn_find(event->subscribe.conn_handle);
        if(c != NULL){
            int idx = chr_by_handle(h);
            if(idx >= 0){
                if(on) c->subs[idx / 8] |=  (uint8_t)(1u << (idx % 8));
                else   c->subs[idx / 8] &= (uint8_t)~(1u << (idx % 8));
            }
        }
        xSemaphoreGive(lock);
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.status == 0){
            CY_LOGI(BLE_DB, "gatt: conn %u encrypted", event->enc_change.conn_handle);
        }else{
            CY_LOGW(BLE_DB, "gatt: conn %u pairing failed, status %d",
                    event->enc_change.conn_handle, event->enc_change.status);
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone lost its bond (forgot the device, reinstalled): drop
         * ours and pair again. */
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0){
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_MTU:
        CY_LOGI(BLE_DB, "gatt: conn %u MTU %d", event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

bool ble_app_gatt_conn_slot_free(void){
    if(!running) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool free_slot = conn_find(BLE_HS_CONN_HANDLE_NONE) != NULL;
    xSemaphoreGive(lock);
    return free_slot;
}

bool ble_app_gatt_running(void){
    return running;
}

/* --------------------------------------------------------- registration --- */

int ble_app_gatt_service_count(void){
    if(cy_ble_services == NULL) return 0;
    int n = cy_ble_service_count;
    return n > CYTHING_BLE_MAX_SERVICES ? CYTHING_BLE_MAX_SERVICES : n;
}

/* 128-bit LE bytes -> NimBLE UUID; Bluetooth-base UUIDs become 16-bit. */
static const uint8_t BT_BASE_LE[12] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00
};

static void uuid_from_table(ble_uuid_any_t *out, const uint8_t le[16]){
    memset(out, 0, sizeof(*out));
    if(memcmp(le, BT_BASE_LE, sizeof(BT_BASE_LE)) == 0 && le[14] == 0 && le[15] == 0){
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value  = (uint16_t)(le[12] | (le[13] << 8));
    }else{
        out->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(out->u128.value, le, 16);
    }
}

static void free_tables(void){
    for(int i = 0 ; i < MAX_CONN ; i++){
        free(conns[i].subs);
        conns[i].subs = NULL;
    }
    free(svc_defs); svc_defs = NULL;
    free(chr_defs); chr_defs = NULL;
    free(uuids);    uuids = NULL;
    free(chr_rt);   chr_rt = NULL;
    svc_total = chr_total = 0;
}

#if BLE_APP_GATT_REQUIRE_ENCRYPTION
#define F_READ   (BLE_GATT_CHR_F_READ  | BLE_GATT_CHR_F_READ_ENC)
#define F_WRITE  (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC)
#else
#define F_READ   (BLE_GATT_CHR_F_READ)
#define F_WRITE  (BLE_GATT_CHR_F_WRITE)
#endif
#define F_NOTIFY (BLE_GATT_CHR_F_NOTIFY)

int ble_app_gatt_register(void){
    int n_svc = ble_app_gatt_service_count();
    if(n_svc == 0) return -1;
    if(cy_ble_service_count > CYTHING_BLE_MAX_SERVICES){
        CY_LOGE(BLE_DB, "gatt: %u services, only the first %d are served",
                cy_ble_service_count, CYTHING_BLE_MAX_SERVICES);
    }

    ble_uuid128_from_base(&auth_svc_uuid, BLE_APP_AUTH_SVC_ID);
    ble_uuid128_from_base(&auth_chr_uuid, BLE_APP_AUTH_CHR_ID);
    ble_uuid128_from_base(&info_chr_uuid, BLE_APP_INFO_CHR_ID);

    /* Count first, so everything is one allocation per array. */
    int n_chr = 0;
    for(int s = 0 ; s < n_svc ; s++){
        const cything_ble_svc_t *svc = &cy_ble_services[s];
        int n = svc->chrs == NULL ? 0 : svc->chr_count;
        if(n > CYTHING_BLE_MAX_CHRS_PER_SERVICE){
            CY_LOGE(BLE_DB, "gatt: service %d has %d characteristics, only %d are served",
                    s, n, CYTHING_BLE_MAX_CHRS_PER_SERVICE);
            n = CYTHING_BLE_MAX_CHRS_PER_SERVICE;
        }
        n_chr += n;
    }

    chr_rt   = calloc(n_chr > 0 ? n_chr : 1, sizeof(*chr_rt));
    uuids    = calloc(n_svc + n_chr, sizeof(*uuids));
    chr_defs = calloc(n_chr + n_svc + 3, sizeof(*chr_defs));   /* + auth, info, {0} */
    svc_defs = calloc(n_svc + 2, sizeof(*svc_defs));
    subs_bytes = (n_chr + 7) / 8 + 1;
    bool ok = chr_rt && uuids && chr_defs && svc_defs;
    for(int i = 0 ; ok && i < MAX_CONN ; i++){
        conns[i].subs = calloc(1, subs_bytes);
        ok = conns[i].subs != NULL;
    }
    if(!ok){
        CY_LOGE(BLE_DB, "gatt: out of memory for %d services / %d characteristics", n_svc, n_chr);
        free_tables();
        return -1;
    }

    int u = 0, c = 0, d = 0, out_svc = 0;
    for(int s = 0 ; s < n_svc ; s++){
        const cything_ble_svc_t *svc = &cy_ble_services[s];
        ble_uuid_any_t *svc_uuid = &uuids[u++];
        uuid_from_table(svc_uuid, svc->uuid128);
        if(ble_uuid_cmp(&svc_uuid->u, &auth_svc_uuid.u) == 0){
            CY_LOGE(BLE_DB, "gatt: service %d uses the library's AUTH slot, skipped", s);
            continue;
        }
        int n = svc->chrs == NULL ? 0 : svc->chr_count;
        if(n > CYTHING_BLE_MAX_CHRS_PER_SERVICE) n = CYTHING_BLE_MAX_CHRS_PER_SERVICE;

        struct ble_gatt_chr_def *first = &chr_defs[d];
        for(int k = 0 ; k < n ; k++){
            const cything_ble_chr_t *chr = &svc->chrs[k];
            if(chr->name == NULL || chr->name[0] == 0 || (chr->flags & 0x07) == 0){
                CY_LOGE(BLE_DB, "gatt: service %d characteristic %d has no name or flags, skipped", s, k);
                continue;
            }
            if(chr_by_name(chr->name) >= 0){
                CY_LOGW(BLE_DB, "gatt: duplicate characteristic name \"%s\": the first one wins", chr->name);
            }
            ble_uuid_any_t *chr_uuid = &uuids[u++];
            uuid_from_table(chr_uuid, chr->uuid128);

            chr_rt[c].def = chr;
            struct ble_gatt_chr_def *def = &chr_defs[d++];
            def->uuid       = &chr_uuid->u;
            def->access_cb  = chr_access;
            def->arg        = (void *)(intptr_t)c;
            def->val_handle = &chr_rt[c].val_handle;
            def->flags      = ((chr->flags & CYTHING_BLE_READ)   ? F_READ   : 0)
                            | ((chr->flags & CYTHING_BLE_WRITE)  ? F_WRITE  : 0)
                            | ((chr->flags & CYTHING_BLE_NOTIFY) ? F_NOTIFY : 0);
            c++;
            chr_total = c;   /* chr_by_name() above sees the ones added so far */
        }
        d++;                 /* the {0} terminator */

        svc_defs[out_svc].type            = BLE_GATT_SVC_TYPE_PRIMARY;
        svc_defs[out_svc].uuid            = &svc_uuid->u;
        svc_defs[out_svc].characteristics = first;
        out_svc++;
    }

    /* The library's AUTH service, last: auth + info. */
    struct ble_gatt_chr_def *auth = &chr_defs[d];
    auth->uuid       = &auth_chr_uuid.u;
    auth->access_cb  = auth_access;
    auth->flags      = F_READ | F_WRITE;
    struct ble_gatt_chr_def *info = &chr_defs[d + 1];
    info->uuid       = &info_chr_uuid.u;
    info->access_cb  = info_access;
    info->flags      = F_READ;
    svc_defs[out_svc].type            = BLE_GATT_SVC_TYPE_PRIMARY;
    svc_defs[out_svc].uuid            = &auth_svc_uuid.u;
    svc_defs[out_svc].characteristics = auth;

    svc_total = out_svc;
    chr_total = c;

    int rc = ble_gatts_count_cfg(svc_defs);
    if(rc == 0) rc = ble_gatts_add_svcs(svc_defs);
    if(rc != 0){
        CY_LOGE(BLE_DB, "gatt: table failed, rc=%d", rc);
        free_tables();
        return -1;
    }

    if(lock == NULL) lock = xSemaphoreCreateMutex();
    for(int i = 0 ; i < MAX_CONN ; i++){
        conns[i].rx = NULL;
        conn_wipe(&conns[i]);
    }

    /* LE Secure Connections, no I/O -> Just Works. Bonding keeps the OS
     * pairing dialog to once per phone. */
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = BLE_APP_GATT_BONDING;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    running = true;
    CY_LOGI(BLE_DB, "gatt: %d services, %d characteristics, %d connections max%s",
            svc_total, chr_total, MAX_CONN, LOCAL_AUTH_ENFORCE == 1 ? ", AUTH enforced" : "");
    return 0;
}
