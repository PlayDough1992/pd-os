#pragma once

/* ============================================================================
 * PD-Kernel  —  Login screen
 * ============================================================================ */

#include "users.h"

/*
 * Show the login screen and prompt for username + password.
 * Allows MAX_ATTEMPTS tries, then halts the CPU on failure.
 * Returns a pointer into the user table on success (never NULL on return).
 */
const user_t *login_prompt(void);
