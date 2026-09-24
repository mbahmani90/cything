#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * The paired list: every phone that has proven the device password and been
 * enrolled (doc/local-auth.md, step 3). One entry per (userSub, installId)
 * — the same account on two phones is two entries, revocable separately.
 *
 * Each entry carries the phone's K_phone, the 32-byte random secret the
 * reconnect handshake (AUTH1..3) is keyed on. Ownership is bound to the
 * account: the list stores an `owner_sub` (the sub of the first phone to
 * pair), and an entry is OWNER iff its user_sub equals it — so a reinstalled
 * or second phone of the owner's account is OWNER again after it re-pairs.
 * The owner-only commands (LIST / REVOKE / PWSET / RESET) check that role.
 *
 * Stored as one NVS blob ("cy_sec" / "paired"): a header + up to
 * PAIRED_LIST_MAX fixed-size entries, rewritten whole on every change.
 * Loaded into RAM once at boot; all reads are served from RAM.
 */

#define PAIRED_LIST_MAX        16
#define PAIRED_SUB_MAX         40   /* Cognito sub is a 36-char UUID */
#define PAIRED_INSTALL_MAX     32
#define PAIRED_NAME_MAX        32
#define PAIRED_KEY_LEN         32

typedef enum {
    PAIRED_ROLE_USER  = 1,
    PAIRED_ROLE_OWNER = 2,
} paired_role_t;

typedef struct {
    char     user_sub[PAIRED_SUB_MAX + 1];
    char     install_id[PAIRED_INSTALL_MAX + 1];
    char     display_name[PAIRED_NAME_MAX + 1];
    uint8_t  key[PAIRED_KEY_LEN];
    uint32_t paired_at;          /* unix seconds, 0 if the clock was not set */
    uint8_t  role;               /* paired_role_t */
} paired_entry_t;

/* Load the list from NVS. Call once from cything_begin() after NVS is up. */
void paired_list_init(void);

int  paired_list_count(void);

/* Index of the entry for (user_sub, install_id), or -1. */
int  paired_list_find(const char *user_sub, const char *install_id);

/* Copy entry `index` into *out. False if the index is out of range. */
bool paired_list_get(int index, paired_entry_t *out);

/* Enroll a phone. `key` is the fresh K_phone (PAIRED_KEY_LEN bytes). If
 * (user_sub, install_id) already exists its key is replaced (re-pairing the
 * same phone); otherwise a new entry is appended. The first phone to pair
 * sets owner_sub; every entry's role then follows owner_sub (OWNER iff its
 * user_sub matches). Strings are truncated to the field sizes. Returns the
 * entry index, or -1 if the list is full / the write failed. `role_out`
 * (optional) receives the entry's role. */
int  paired_list_add(const char *user_sub, const char *install_id,
                     const char *display_name, const uint8_t *key,
                     uint32_t paired_at, paired_role_t *role_out);

/* Remove entry `index`. The caller enforces the owner rule. */
esp_err_t paired_list_remove(int index);

/* Wipe the whole list (RESET). */
esp_err_t paired_list_clear(void);

/* If no entry is the owner but the list is non-empty, promote entry 0 to
 * owner and persist. Called after a self-unpair that removed the owner, so
 * the device is never left with users but no one who can manage it. No-op
 * when an owner already exists or the list is empty. */
esp_err_t paired_list_promote_owner(void);
