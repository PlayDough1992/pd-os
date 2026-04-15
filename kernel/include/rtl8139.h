#pragma once

/* ============================================================================
 * PD-Kernel  —  RTL8139 Fast Ethernet driver
 * ============================================================================ */

#include "kernel.h"

/* Maximum Ethernet payload (1500 data + 14 header = 1514 bytes) */
#define NET_MTU  1514

/*
 * rtl8139_init() — PCI scan + hardware init.
 * Must be called after paging_init() and kheap_init(), before sti().
 * Gracefully does nothing if no RTL8139 is found.
 */
void    rtl8139_init(void);

/* Called from IRQ dispatcher when the NIC's IRQ fires. */
void    rtl8139_handler(void);

/* Returns 1 if an RTL8139 was found and initialized, 0 otherwise. */
int     rtl8139_present(void);

/* Returns the PCI interrupt line number (e.g. 11 in standard QEMU). */
uint8_t rtl8139_get_irq(void);

/* Copies the 6-byte MAC address into mac[]. */
void    rtl8139_get_mac(uint8_t mac[6]);

/*
 * rtl8139_send() — transmit a raw Ethernet frame.
 * Returns  0 on success, -1 if the card is absent or the TX ring is stalled.
 * Non-blocking: the actual DMA TX happens in the background.
 */
int     rtl8139_send(const void *data, uint16_t len);

/*
 * rtl8139_recv() — pull the next received frame from the software queue.
 * Copies up to NET_MTU bytes into buf and writes the frame length to *out_len.
 * Returns 1 if a packet was available, 0 if the receive queue is empty.
 */
int     rtl8139_recv(void *buf, uint16_t *out_len);
