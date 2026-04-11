#pragma once

/* ============================================================================
 * PD-Kernel  —  PS/2 Keyboard driver
 * ============================================================================ */

#include "kernel.h"

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_getchar(void);    /* blocking read of next ASCII char */
