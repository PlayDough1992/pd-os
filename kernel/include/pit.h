#pragma once

/* ============================================================================
 * PD-Kernel  —  PIT (8253/8254 Programmable Interval Timer)
 * ============================================================================ */

#include "kernel.h"

void pit_init(uint32_t frequency_hz);
uint32_t pit_get_ticks(void);
void pit_handler(void);
