#pragma once

/* ============================================================================
 * PD-Kernel  —  USB EHCI + Mass Storage Class driver
 *
 * Discovers the first USB 2.0 high-speed mass storage device reachable via
 * an EHCI host controller and exposes it as a block device so the ATA driver
 * (and therefore the filesystem stack) can transparently read/write sectors
 * on the boot USB drive.  This is the self-hosting path: no ATA/IDE hardware
 * is required once a USB drive is present.
 * ============================================================================ */

#include "kernel.h"

/*
 * Scan PCI for EHCI controllers, perform BIOS handoff, reset and start the
 * host controller, enumerate all ports, and identify the first USB 2.0
 * high-speed mass storage device found.
 *
 * Returns  0 if a mass storage device was found and is ready.
 * Returns -1 if no suitable USB device was found.
 */
int usb_init(void);

/* Returns 1 if a USB mass storage device is ready, 0 otherwise. */
int usb_available(void);

/*
 * Read/write one 512-byte sector from/to the USB mass storage device.
 * 'buf' must be at least 512 bytes.
 *
 * These are called by ata_read_sectors() / ata_write_sectors() when
 * usb_available() returns 1, making the USB drive transparent to all
 * filesystem drivers that go through the ATA API.
 *
 * Returns  0 on success.
 * Returns -1 on SCSI / transport error.
 */
int usb_read_sector (uint32_t lba, void       *buf);
int usb_write_sector(uint32_t lba, const void *buf);

/*
 * Poll the USB HID keyboard endpoint and inject any newly-pressed keys into
 * the kernel keyboard ring buffer via keyboard_inject().  Safe to call at any
 * time; returns immediately if no USB keyboard was found during enumeration.
 *
 * Called from keyboard_getchar() (inside the hlt wait loop) and from
 * keyboard_poll() so both blocking and non-blocking paths work.
 */
void usb_keyboard_poll(void);
