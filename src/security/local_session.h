#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Per-socket state for the local (TCP) link: line framing plus the
 * authentication state machine described in doc/local-auth.md.
 *
 * One fixed pool of LOCAL_SESSION_MAX slots. A slot is acquired by the
 * listener on accept() and released by the client's receive task when the
 * socket goes away. Slots never move — unlike account_struct_list, which is
 * compacted on every disconnect — so a task may hold a pointer to its own
 * slot for the life of the connection.
 *
 * The pool is deliberately separate from the client registry
 * (tcp_client_list.c): that list is what the send task iterates and has no
 * per-connection secrets; this pool is where keys live. Releasing a slot
 * wipes its key material.
 */

#define LOCAL_SESSION_MAX 8

/* Where a connection is in the handshake. Transitions are owned by the
 * security handlers (pake_handler.c / local_auth.c); everything else only
 * reads the state. */
typedef enum {
    LS_NEW = 0,          /* accepted, nothing exchanged                           */
    LS_PAKE_WAIT_PROOF,  /* PAKE1 seen, B sent, waiting for the client's M1       */
    LS_PAKE_OK,          /* M1/M2 verified: `key` is K_pake, ENROLL allowed       */
    LS_AUTH_WAIT_PROOF,  /* AUTH1 seen, AUTH2 sent, waiting for the phone's proof */
    LS_AUTH_OK,          /* proof verified: `key` is K_session                    */
} local_session_state_t;

typedef enum {
    LS_ROLE_NONE = 0,
    LS_ROLE_USER,
    LS_ROLE_OWNER,
} local_session_role_t;

typedef struct {
    int      sock;                 /* -1 = free slot */
    uint32_t peer_ip;              /* IPv4, network order; per-IP rate limiting */

    /* Line framing: bytes buffered so far for the line in progress.
     * Was a single global shared by every client task. */
    int      rx_len;

    /* Handshake state (doc/local-auth.md). */
    local_session_state_t state;
    local_session_role_t  role;
    int      paired_index;         /* row in the paired list, -1 until known */
    void    *srp;                  /* esp_srp_handle_t*, only between PAKE1 and PAKE3 */

    uint8_t  key[32];              /* K_pake (after PAKE) or K_session (after AUTH) */
    uint8_t  nonce_dev[16];
    uint8_t  nonce_phone[16];
    uint64_t tx_ctr;               /* GCM counter nonces, one per direction */
    uint64_t rx_ctr;
    bool     rx_encrypted;         /* the line being dispatched arrived in an ENC: frame */

    /* This connection ran the anyone-can-PWSET bootstrap route (device had no
     * password at the time). Lets it PWCLEAR back to open once it has enrolled
     * itself, even without the OWNER role — see owner_commands.c dispatch —
     * so a non-owner visitor that self-records on an open device can hand back
     * the open state it (temporarily) borrowed, without ever being able to
     * open a device it did not itself un-password. */
    bool     bootstrap_claim;
} local_session_t;

/* Call once from cything_begin() before the TCP server starts. */
void local_session_init(void);

/* Reserve a slot for a freshly accepted socket. NULL if the pool is full —
 * the listener should refuse the connection, as it does for a full client
 * list. The slot comes back zeroed with state LS_NEW. */
local_session_t *local_session_acquire(int sock, uint32_t peer_ip);

/* Slot for `sock`, or NULL if the socket has no session (never acquired or
 * already released). */
local_session_t *local_session_find(int sock);

/* Free the slot for `sock`: frees a live SRP handle, wipes the keys, marks
 * the slot free. No-op if `sock` has no session. */
void local_session_release(int sock);

/* Number of slots in use (diagnostics). */
int local_session_count(void);

/* The paired list dropped entry `index` (REVOKE): any session on that entry
 * loses its authentication (key wiped, back to LS_NEW — its next line
 * fails and the phone has to re-pair), and sessions on later entries have
 * their paired_index shifted down to follow the compacted list. */
void local_session_on_paired_removed(int index);

/* Every session loses its authentication (RESET / PWSET). */
void local_session_drop_all_auth(void);

/* Abandon any in-progress or established handshake on this socket and return
 * it to LS_NEW (SRP handle freed, key + nonces wiped, counters and role
 * cleared). Called at the start of PAKE1 / AUTH1 so a fresh handshake line
 * always means "start over" — otherwise a socket left mid-handshake (e.g. a
 * client that aborted an AUTH on a device-proof mismatch, stuck in
 * LS_AUTH_WAIT_PROOF) would refuse every later PAKE1/AUTH1 with ERR:BUSY.
 * The caller owns this slot's fields (its own recv task), so no lock. */
void local_session_reset_handshake(local_session_t *s);
