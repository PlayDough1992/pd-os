#pragma once

/* ============================================================================
 * PD-Kernel  —  Filesystem population + Desktop Environment launcher
 *               (Phase 11)
 *
 * fs_populate():
 *   Called once from kernel_main after pdfs_mount() succeeds.
 *   Creates the standard PD-OS directory tree (Debian hierarchy + /de)
 *   and writes /etc/pd-os/version and /etc/passwd if they do not exist.
 *   Idempotent: every mkdir / create call is preceded by a vfs_open check,
 *   so it is safe to call on every boot.
 *
 * de_select_and_launch():
 *   Called from kernel_main after a successful login, before the shell loop.
 *   Scans /de/ for installed desktop environments and either auto-launches
 *   the sole DE, loads the default named in /de/default, or prompts the user
 *   to choose one (or skip to PD-Shell).
 *   Phase 11 stub: launches a banner message and returns; binary execution
 *   is implemented in Phase 12 (kernel ELF loader).
 * ============================================================================ */

#include "users.h"

/*
 * Create the standard directory tree and seed system files.
 * Must be called after vfs_mount("/", "pdfs", 200) returns 0.
 */
void fs_populate(void);

/*
 * Scan /de/, run DE selection logic, and (Phase 11 stub) print a launch
 * banner if a DE is chosen.  Caller is the authenticated user.
 * Returns without blocking; returns immediately if no DEs are installed.
 */
void de_select_and_launch(const user_t *user);
