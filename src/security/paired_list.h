#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * The paired list: every account that has proven the device password and
 * been enrolled (doc/local-auth.md, step 3). One entry per userSub — every
 * phone of an account shares the entry and its key (K_account), which the
 * app escrows in the backend so the account's other phones fetch it instead
 * of re-pairing. `user_email` is a label the app supplies at ENROLL so the
 * owner can tell accounts apart in LIST; it is asserted, not verified —
 * `user_sub` is the identity.
 *
 * Each entry carries K_account, the 32-byte random secret the reconnect
 * handshake (AUTH1..3) is keyed on. Ownership is bound to the account: the
 * list stores an `owner_sub` (the sub of the first account to pair), and an
 * entry is OWNER iff its user_sub equals it. The owner-only commands
 * (LIST / REVOKE / PWSET / RESET) check that role.
 *
 * Stored as one NVS blob ("cy_sec" / "paired"): a header + up to
 * PAIRED_LIST_MAX fixed-size entries, rewritten whole on every change.
 * Loaded into RAM once at boot; all reads are served from RAM.
 */

#define PAIRED_LIST_MAX        16
#define PAIRED_SUB_MAX         40   /* Cognito sub is a 36-char UUID */
#define PAIRED_EMAIL_MAX       64
#define PAIRED_KEY_LEN         32

typedef enum {
    PAIRED_ROLE_USER  = 1,
    PAIRED_ROLE_OWNER = 2,
} paired_role_t;

typedef struct {
    char     user_sub[PAIRED_SUB_MAX + 1];
    char     user_email[PAIRED_EMAIL_MAX + 1];   /* label only, may be empty */
    uint8_t  key[PAIRED_KEY_LEN];
    uint32_t paired_at;          /* unix seconds, 0 if the clock was not set */
    uint8_t  role;               /* paired_role_t */
} paired_entry_t;

/* Load the list from NVS. Call once from cything_begin() after NVS is up. */
void paired_list_init(void);

int  paired_list_count(void);

/* Index of the entry for `user_sub`, or -1. */
int  paired_list_find(const char *user_sub);

/* Copy entry `index` into *out. False if the index is out of range. */
bool paired_list_get(int index, paired_entry_t *out);

/* Enroll an account. `key` (PAIRED_KEY_LEN bytes) is in/out: a fresh random
 * key on entry. If `user_sub` has no entry, one is appended with that key.
 * If it already has one, the account's key is kept — never rotated here, or
 * its other phones and the backend escrow would stop working — and copied
 * into `key`. The first account to pair sets owner_sub; every entry's role
 * then follows owner_sub. A non-empty `user_email` replaces the stored
 * one (NULL / empty keeps it). Strings are truncated to the field sizes. Atomic,
 * so two phones of one account enrolling at once get the same key. Returns
 * the entry index, or -1 if the list is full / the write failed. `role_out`
 * (optional) receives the entry's role. */
int  paired_list_add(const char *user_sub, const char *user_email,
                     uint8_t key[PAIRED_KEY_LEN],
                     uint32_t paired_at, paired_role_t *role_out);

/* Give the OWNER entry (the owning account) a fresh random key. Called on
 * PWSET: a key handed out while the device was open — to anyone who claimed
 * the owner's sub — stops working once the owner locks it. The owner's phones
 * re-enroll with the new password (or fetch the new key from the escrow). */
esp_err_t paired_list_rotate_owner_key(void);

/* Remove entry `index`. The caller enforces the owner rule. */
esp_err_t paired_list_remove(int index);

/* Wipe the whole list (RESET). */
esp_err_t paired_list_clear(void);

