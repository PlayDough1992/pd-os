#pragma once

/* ============================================================================
 * PD-Kernel  —  User account system
 * ============================================================================ */

#include "kernel.h"

#define MAX_USERS    16
#define USERNAME_LEN 32

#define USER_FLAG_ROOT  0x01

typedef struct {
    char     username[USERNAME_LEN];
    uint32_t password_hash;
    uint8_t  uid;
    uint8_t  flags;
} user_t;

void          users_init(void);
uint32_t      users_hash(const char *s);
const user_t *users_get(const char *username);
int           users_verify(const char *username, const char *password);

/*
 * Add a new user.  Returns 0 on success, -1 table full, -2 bad name, -3 exists.
 * is_admin != 0 → USER_FLAG_ROOT set.  Password is hashed immediately.
 */
int           users_add(const char *username, uint8_t is_admin, const char *password);

/*
 * Remove a user by username.  Returns 0 on success, -1 not found, -2 root.
 * Cannot remove uid 0 (root) or the current session user (caller must check).
 */
int           users_remove(const char *username);

/*
 * Change the password for `username`.  The new hash replaces the old one.
 * Returns 0 on success, -1 if user not found.
 */
int           users_change_password(const char *username, const char *new_password);

/*
 * Persist one user's credentials to /usr/<uid>.pduc (root-owned, mode 0600).
 * Must be called after users_add / users_change_password so the new hash is
 * stored on disk and survives reboot.  Runs elevated internally.
 * Returns 0 on success, negative on error.
 */
int           users_save_to_disk(const char *username);

/*
 * On boot (after PDFS mounts): scan /usr for *.pduc files and populate the
 * in-memory user table.  Hardcoded UIDs 0 (root) and 1 (pd) have their
 * password hashes updated from disk if a matching .pduc exists (so a
 * changerpass / changempass across reboots is also persistent).  UIDs 2+
 * are added as new user_table entries.  Runs elevated internally.
 */
void          users_load_from_disk(void);
