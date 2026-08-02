/* ============================================================================
 * PD-Kernel  —  PCI Configuration Space Access
 *
 * Implements standard type-1 PCI configuration space I/O (ports 0xCF8/0xCFC).
 * Extracted and generalised from the RTL8139 driver.
 * ============================================================================ */

#include "pci.h"
#include "kernel.h"

/* ---- I/O port helpers (needed here; not shared from io.h) ---------------- */
static inline void _pci_outl(uint16_t p, uint32_t v)
{ __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(p)); }

static inline uint32_t _pci_inl(uint16_t p)
{ uint32_t r; __asm__ volatile ("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }

/* ---- Build the 32-bit PCI address register value ------------------------- */
static inline uint32_t pci_addr(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    return (1u << 31)
         | ((uint32_t)b          << 16)
         | ((uint32_t)(d & 0x1F) << 11)
         | ((uint32_t)(f & 0x07) <<  8)
         | (off & 0xFC);
}

/* ---- Public read/write functions ----------------------------------------- */

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    _pci_outl(0xCF8, pci_addr(b, d, f, off));
    return _pci_inl(0xCFC);
}

uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    uint32_t r = pci_read32(b, d, f, off & ~1u);
    return (uint16_t)((r >> ((off & 2u) * 8u)) & 0xFFFFu);
}

uint8_t pci_read8(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    uint32_t r = pci_read32(b, d, f, off & ~3u);
    return (uint8_t)((r >> ((off & 3u) * 8u)) & 0xFFu);
}

void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v)
{
    _pci_outl(0xCF8, pci_addr(b, d, f, off));
    _pci_outl(0xCFC, v);
}

void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint16_t v)
{
    uint32_t cur   = pci_read32(b, d, f, off & ~1u);
    uint32_t shift = (off & 2u) * 8u;
    pci_write32(b, d, f, off & ~1u,
                (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift));
}

/* ---- Device search by class code ----------------------------------------- */

int pci_find_class(uint8_t cc, uint8_t sc, uint8_t pi,
                   uint8_t *bus_io, uint8_t *dev_io, uint8_t *func_io)
{
    uint16_t b, d, f;

    for (b = *bus_io; b < 256; b++) {
        uint16_t d0 = (b == *bus_io) ? *dev_io : 0;
        for (d = d0; d < 32; d++) {
            /* Probe function 0 first — if absent, skip whole device */
            uint32_t id0 = pci_read32((uint8_t)b, (uint8_t)d, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;

            /* Determine number of functions */
            uint8_t ht   = pci_read8((uint8_t)b, (uint8_t)d, 0, 0x0E);
            uint8_t maxf = (ht & 0x80) ? 8 : 1;

            uint8_t f0 = (b == *bus_io && d == d0) ? *func_io : 0;
            for (f = f0; f < maxf; f++) {
                uint32_t id = (f == 0) ? id0
                            : pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint32_t cls = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 8);
                if (((cls >> 24) & 0xFF) == cc &&
                    ((cls >> 16) & 0xFF) == sc &&
                    ((cls >>  8) & 0xFF) == pi) {
                    *bus_io  = (uint8_t)b;
                    *dev_io  = (uint8_t)d;
                    *func_io = (uint8_t)f;
                    return 1;
                }
            }
        }
    }
    return 0;
}
