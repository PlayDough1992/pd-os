#pragma once

/* ============================================================================
 * PD-OS  —  Installation Wizard
 * ============================================================================
 * Invoked by cmd_install in shell.c.
 * Draws a full-screen VGA TUI, copies the running system image to a selected
 * ATA drive, zeroes the PDFS header so it auto-initialises on first boot,
 * then prompts the user to reboot.
 * ============================================================================ */

#include "kernel.h"

void install_wizard(void);
