#pragma once

/* ============================================================================
 * PD-Kernel  —  Kernel I/O (formatted output)
 * ============================================================================ */

void kputs(const char *s);
void kprintf(const char *fmt, ...);

/* Redirect all kprintf/kputs output to a custom character writer.
 * Pass NULL to revert to VGA text mode.  Called after gfx_init(). */
void kprint_redirect(void (*fn)(char));
