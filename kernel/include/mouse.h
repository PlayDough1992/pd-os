#ifndef MOUSE_H
#define MOUSE_H

#include "kernel.h"

/* Initialise PS/2 mouse (enable aux device, enable IRQ12) */
void mouse_init(void);

/* Called from IRQ12 handler — reads one packet byte and updates state */
void mouse_handler(void);

/* Current mouse state (updated each complete 3-byte packet) */
void mouse_get_pos(int *x, int *y);
uint8_t mouse_get_buttons(void);   /* bit 0=left, bit 1=right, bit 2=middle */

/* Register a callback invoked on every completed packet */
typedef void (*mouse_callback_t)(int x, int y, uint8_t buttons);
void mouse_set_callback(mouse_callback_t cb);

#endif /* MOUSE_H */
