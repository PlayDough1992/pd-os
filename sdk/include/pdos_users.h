#pragma once

/* ============================================================================
 * PD-OS DE SDK  —  pdos_users.h
 * User account type visible to DE binaries.
 * ============================================================================ */

#include "pdos_types.h"

#define MAX_USERS    16
#define USERNAME_LEN 32
#define USER_FLAG_ROOT  0x01

typedef struct {
    char     username[USERNAME_LEN];
    uint32_t password_hash;
    uint8_t  uid;
    uint8_t  flags;
} user_t;
