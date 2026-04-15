#pragma once

/* ============================================================================
 * PD-Kernel  —  PS/2 Keyboard driver
 * ============================================================================ */

#include "kernel.h"

/* Special key codes returned by keyboard_getchar() */
#define KEY_UP    '\x11'
#define KEY_DOWN  '\x12'
#define KEY_LEFT  '\x13'
#define KEY_RIGHT '\x14'
#define KEY_PGUP  '\x15'
#define KEY_PGDN  '\x16'

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_getchar(void);
char keyboard_poll(void);   /* non-blocking; returns 0 if no key waiting */
