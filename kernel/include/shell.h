#pragma once

/* ============================================================================
 * PD-Shell  —  Built-in command shell (Tier 1)
 * ============================================================================ */

#include "kernel.h"
#include "users.h"

#define SHELL_BUF_SIZE  512
#define SHELL_MAX_ARGS  16

/*
 * Run the shell for the given logged-in user.
 * Returns when the user runs 'logout'.
 */
void shell_run(const user_t *user);
