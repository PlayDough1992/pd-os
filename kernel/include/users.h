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
