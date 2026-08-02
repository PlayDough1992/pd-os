#pragma once

/* ============================================================================
 * PD-Kernel  —  PCI Configuration Space
 *
 * Shared access helpers extracted from rtl8139.c so that multiple drivers
 * (RTL8139, USB EHCI) can use PCI without duplicating the code.
 * ============================================================================ */

#include "kernel.h"

/* Read PCI configuration space (32 / 16 / 8 bit) */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);

/* Write PCI configuration space (32 / 16 bit) */
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t val);

/*
 * Search for a PCI device by class code / subclass / programming interface.
 * Starts the search from position (*bus, *dev, *func) inclusive.
 * On success, fills (*bus, *dev, *func) with the device's position.
 * Returns 1 if found, 0 if the end of the bus was reached.
 *
 * To iterate over all matching devices, increment *func (wrapping dev/bus as
 * needed) and call again after each successful return.
 */
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                   uint8_t *bus, uint8_t *dev, uint8_t *func);
