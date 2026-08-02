#pragma once

/* ============================================================================
 * PD-OS GDE  —  PS/2 mouse driver
 * ============================================================================ */

#include "kernel.h"

void    mouse_init(void);
void    mouse_handler(void);   /* called from IRQ12 */
void    mouse_poll(void);      /* drain 8042 AUX buffer; call from event loops */

int     mouse_get_x(void);
int     mouse_get_y(void);
uint8_t mouse_get_buttons(void);  /* bit0=left, bit1=right, bit2=middle */

/* Returns 1 if mouse state changed since last call to mouse_clear_changed() */
int     mouse_changed(void);
void    mouse_clear_changed(void);

/* Press latch: bits that transitioned 0→1 since last mouse_clear_btn_latch().
 * Use this instead of `buttons & ~prev` to catch rapid clicks that complete
 * (down+up) between two event-loop iterations. */
uint8_t mouse_get_btn_latch(void);
void    mouse_clear_btn_latch(void);
