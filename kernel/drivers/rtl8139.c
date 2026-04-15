/* ============================================================================
 * PD-Kernel  —  RTL8139 Fast Ethernet Driver
 *
 * Supports QEMU's emulated RTL8139C (PCI vendor 0x10EC, device 0x8139).
 * Uses I/O port mapped registers (BAR0) and fixed-address DMA buffers.
 *
 * Physical memory layout (mapped as a 4 MB PSE large page at PDE[3]):
 *   0xC00000 – 0xC025F7   RX ring buffer  (8192 + 16 + 1500 bytes)
 *   0xC04000 – 0xC05FFF   TX buffer 0     (2048 bytes)
 *   0xC06000 – 0xC07FFF   TX buffer 1     (2048 bytes)
 *   0xC08000 – 0xC09FFF   TX buffer 2     (2048 bytes)
 *   0xC0A000 – 0xC0BFFF   TX buffer 3     (2048 bytes)
 * ============================================================================ */

#include "rtl8139.h"
#include "paging.h"
#include "pic.h"
#include "io.h"

/* ---- DMA buffer physical addresses --------------------------------------- */
/* 4 MB PSE page at 0xC00000 (PDE index 3) holds all NIC DMA regions.       */
#define NET_RX_BUF_PHYS   0xC00000u
#define NET_RX_BUF_SIZE   8192u                     /* ring size (power-of-2) */
#define NET_RX_BUF_ALLOC  (NET_RX_BUF_SIZE + 16u + 1500u) /* alloc w/ margin */
#define NET_TX_BUF_PHYS   0xC04000u                 /* 4 K-aligned TX buffers */
#define NET_TX_BUF_STRIDE 0x2000u                   /* 8 KB per descriptor    */

/* ---- RTL8139 I/O register offsets ---------------------------------------- */
#define RTL_IDR0    0x00u   /* MAC address bytes 0-5                          */
#define RTL_TSD0    0x10u   /* TX Status Descriptor 0  (32-bit, × 4)         */
#define RTL_TSAD0   0x20u   /* TX Start Address 0      (32-bit, × 4)         */
#define RTL_RBSTART 0x30u   /* RX Buffer Start Address (32-bit physical)      */
#define RTL_ERBCR   0x34u   /* Early RX Byte Count Register                   */
#define RTL_ERSR    0x36u   /* Early RX Status Register                       */
#define RTL_CR      0x37u   /* Command Register                               */
#define RTL_CAPR    0x38u   /* Current Address of Packet Read (16-bit)        */
#define RTL_CBR     0x3Au   /* Current Buffer Address (16-bit, NIC write pos) */
#define RTL_IMR     0x3Cu   /* Interrupt Mask Register  (16-bit)              */
#define RTL_ISR     0x3Eu   /* Interrupt Status Register (16-bit)             */
#define RTL_TCR     0x40u   /* TX Configuration Register (32-bit)             */
#define RTL_RCR     0x44u   /* RX Configuration Register (32-bit)             */
#define RTL_9346CR  0x50u   /* 93C46 / Config unlock register                 */
#define RTL_CONFIG1 0x52u   /* Configuration Register 1                       */

/* ---- Command Register bits ----------------------------------------------- */
#define RTL_CR_BUFE (1u << 0)   /* RX buffer empty                           */
#define RTL_CR_TE   (1u << 2)   /* TX enable                                 */
#define RTL_CR_RE   (1u << 3)   /* RX enable                                 */
#define RTL_CR_RST  (1u << 4)   /* Software reset (self-clearing)            */

/* ---- Interrupt status/mask bits ------------------------------------------ */
#define RTL_INT_ROK   (1u << 0)   /* RX OK                                   */
#define RTL_INT_RER   (1u << 1)   /* RX Error                                */
#define RTL_INT_TOK   (1u << 2)   /* TX OK                                   */
#define RTL_INT_TER   (1u << 3)   /* TX Error                                */
#define RTL_INT_RXOVW (1u << 4)   /* RX Buffer Overflow                      */
#define RTL_INT_FOVW  (1u << 6)   /* RX FIFO Overflow                        */

/* ---- TX Status Descriptor bits ------------------------------------------- */
#define RTL_TSD_SIZE_MASK 0x1FFFu   /* bits 12:0 = packet size               */
#define RTL_TSD_OWN       (1u << 13) /* 1 = driver owns (slot free for TX)   */
#define RTL_TSD_TOK       (1u << 15) /* TX OK                                */
#define RTL_TSD_TER       (1u << 30) /* TX Aborted / error                   */

/* ---- RX Configuration Register bits ------------------------------------- */
#define RTL_RCR_AAP       (1u << 0)   /* Accept All Packets (promiscuous)    */
#define RTL_RCR_APM       (1u << 1)   /* Accept Physical address Match       */
#define RTL_RCR_AM        (1u << 2)   /* Accept Multicast                    */
#define RTL_RCR_AB        (1u << 3)   /* Accept Broadcast                    */
#define RTL_RCR_WRAP      (1u << 7)   /* Wrap ring-buffer (don't stop at end)*/
/* RBLEN bits 12:11 = 0  → 8 K + 16 bytes (left at reset default)            */
/* RXFTH bits 15:13 = 7  → no RX FIFO threshold; copy whole packet at once   */
#define RTL_RCR_RXFTH_ALL (7u << 13)
/* MXDMA bits 10:8  = 6  → 1024-byte max DMA burst                           */
#define RTL_RCR_MXDMA_1K  (6u << 8)

/* ---- TX Configuration Register bits ------------------------------------- */
/* IFG  bits 25:24 = 3  → standard 96-bit interframe gap                     */
#define RTL_TCR_IFG_STD   (3u << 24)
/* MXDMA bits 10:8 = 6  → 1024-byte max DMA burst                            */
#define RTL_TCR_MXDMA_1K  (6u << 8)

/* ---- Software RX packet queue -------------------------------------------- */
#define NET_RXQUEUE_SLOTS  8   /* must be a power of 2                        */

static uint8_t  g_rxq_data[NET_RXQUEUE_SLOTS][NET_MTU];
static uint16_t g_rxq_len [NET_RXQUEUE_SLOTS];
static int      g_rxq_head = 0;   /* consumer index                          */
static int      g_rxq_tail = 0;   /* producer index (ISR-written)            */

/* ---- Driver state -------------------------------------------------------- */
static int      g_rtl_present = 0;
static uint16_t g_iobase      = 0;
static uint8_t  g_irq         = 0;
static uint8_t  g_mac[6];
static int      g_tx_cur      = 0;   /* next TX descriptor index (0–3)       */
static uint32_t g_rx_offset   = 0;   /* byte offset into RX ring buffer      */

/* ============================================================================
 * Low-level I/O helpers
 * (mirrors what ATA and mouse drivers do; no shared header for I/O primitives)
 * ============================================================================ */
static inline void outb(uint16_t port, uint8_t v)
{ __asm__ volatile ("outb %0,%1"::"a"(v),"Nd"(port)); }

static inline void outw(uint16_t port, uint16_t v)
{ __asm__ volatile ("outw %0,%1"::"a"(v),"Nd"(port)); }

static inline void outl(uint16_t port, uint32_t v)
{ __asm__ volatile ("outl %0,%1"::"a"(v),"Nd"(port)); }

static inline uint8_t  inb(uint16_t port)
{ uint8_t  r; __asm__ volatile ("inb %1,%0":"=a"(r):"Nd"(port)); return r; }

static inline uint16_t inw(uint16_t port)
{ uint16_t r; __asm__ volatile ("inw %1,%0":"=a"(r):"Nd"(port)); return r; }

static inline uint32_t inl(uint16_t port)
{ uint32_t r; __asm__ volatile ("inl %1,%0":"=a"(r):"Nd"(port)); return r; }

/* ============================================================================
 * Minimal PCI config space access (ports 0xCF8 / 0xCFC)
 * ============================================================================ */
static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}

static void pci_write(uint8_t bus, uint8_t slot, uint8_t func,
                      uint8_t off, uint32_t val)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func <<  8)
                  | (off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, val);
}

/* ============================================================================
 * RX ring buffer helpers
 * ============================================================================ */

/*
 * ring_copy — copy len bytes starting at ring_offset from the RX ring into dst.
 * Handles wrap-around: if the source crosses the 8 K boundary, the second part
 * is read from the start of the ring.
 */
static void ring_copy(uint8_t *dst, uint32_t ring_offset, uint16_t len)
{
    const uint8_t *ring = (const uint8_t *)NET_RX_BUF_PHYS;
    uint32_t       end  = ring_offset + (uint32_t)len;

    if (end <= NET_RX_BUF_SIZE) {
        /* Contiguous — fast path */
        uint16_t i;
        for (i = 0; i < len; i++)
            dst[i] = ring[ring_offset + i];
    } else {
        /* Wrap-around — two-part copy */
        uint16_t part1 = (uint16_t)(NET_RX_BUF_SIZE - ring_offset);
        uint16_t part2 = (uint16_t)(len - part1);
        uint16_t i;
        for (i = 0; i < part1; i++) dst[i]        = ring[ring_offset + i];
        for (i = 0; i < part2; i++) dst[part1 + i] = ring[i];
    }
}

/* ============================================================================
 * RX processing — called from the ISR on RTL_INT_ROK
 * ============================================================================ */
static void rtl8139_do_rx(void)
{
    /* Poll until the RX buffer-empty bit is set (no more packets) */
    while (!(inb(g_iobase + RTL_CR) & RTL_CR_BUFE)) {

        /* The 4-byte packet header sits at ring[g_rx_offset]:
         *   bytes 0-1  status word  (bit 0 = ROK)
         *   bytes 2-3  total length (Ethernet frame INCLUDING 4-byte CRC) */
        const uint8_t *ring = (const uint8_t *)NET_RX_BUF_PHYS;
        uint32_t       off  = g_rx_offset;

        uint16_t pkt_status = (uint16_t)(ring[off] | ((uint16_t)ring[off + 1] << 8));
        uint16_t pkt_len    = (uint16_t)(ring[off + 2] | ((uint16_t)ring[off + 3] << 8));

        /* Sanity: ROK must be set; length must be plausible */
        if (!(pkt_status & 1u) || pkt_len < 8u || pkt_len > 1518u) {
            /* Bad packet — reset CAPR to CBR to re-sync, then break */
            outw(g_iobase + RTL_CAPR,
                 (uint16_t)((inw(g_iobase + RTL_CBR) - 16u) & 0xFFFFu));
            g_rx_offset = (uint32_t)(inw(g_iobase + RTL_CBR)) & (NET_RX_BUF_SIZE - 1u);
            break;
        }

        /* Payload = frame without the trailing 4-byte CRC */
        uint16_t data_len = (uint16_t)(pkt_len - 4u);

        /* Enqueue; silently drop if the software ring is full */
        int next = (g_rxq_tail + 1) & (NET_RXQUEUE_SLOTS - 1);
        if (next != g_rxq_head && data_len <= NET_MTU) {
            /* Packet data starts 4 bytes after the header */
            ring_copy(g_rxq_data[g_rxq_tail],
                      (off + 4u) & (NET_RX_BUF_SIZE - 1u),
                      data_len);
            g_rxq_len[g_rxq_tail] = data_len;
            g_rxq_tail = next;
        }

        /* Advance the read pointer: 4-byte header + total pkt_len, aligned to 4 */
        uint32_t advance = (4u + (uint32_t)pkt_len + 3u) & ~3u;
        g_rx_offset      = (g_rx_offset + advance) & (NET_RX_BUF_SIZE - 1u);

        /* Tell the NIC where we read up to.
         * RTL8139 quirk: CAPR = actual_offset - 16 (leaves a 16-byte safety gap). */
        outw(g_iobase + RTL_CAPR,
             (uint16_t)((g_rx_offset - 16u) & 0xFFFFu));
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void rtl8139_init(void)
{
    uint8_t  bus  = 0;
    uint8_t  slot = 0;
    uint8_t  func = 0;
    int      found = 0;

    /* ---- PCI scan: look for Realtek 8139 (vendor 0x10EC, device 0x8139) -- */
    for (bus = 0; bus < 8u && !found; bus++) {
        for (slot = 0; slot < 32u && !found; slot++) {
            uint32_t id = pci_read(bus, slot, func, 0x00u);
            if ((id & 0xFFFFu) != 0x10ECu) continue;  /* vendor */
            if ((id >> 16)     != 0x8139u) continue;  /* device */
            found = 1;
        }
    }

    if (!found) {
        kprintf("  (!) RTL8139: not present\n");
        return;
    }

    /* undo the for-loop post-increment */
    bus--;
    slot--;

    /* ---- Read BAR0 (I/O base) and interrupt line ------------------------- */
    uint32_t bar0 = pci_read(bus, slot, func, 0x10u);
    g_iobase = (uint16_t)(bar0 & ~0x3u);          /* clear I/O indicator bits */

    uint32_t irq_reg = pci_read(bus, slot, func, 0x3Cu);
    g_irq = (uint8_t)(irq_reg & 0xFFu);

    /* ---- Enable PCI I/O space + bus mastering ---------------------------- */
    uint32_t pci_cmd = pci_read(bus, slot, func, 0x04u);
    pci_write(bus, slot, func, 0x04u, pci_cmd | 0x07u); /* I/O + Mem + BusMaster */

    /* ---- Map DMA buffer region (4 MB PSE page at PDE[3] = 0xC00000) ----- */
    paging_map_framebuffer(NET_RX_BUF_PHYS);

    /* Zero the RX ring buffer */
    {
        volatile uint32_t *p = (volatile uint32_t *)NET_RX_BUF_PHYS;
        uint32_t           i;
        for (i = 0; i < (NET_RX_BUF_ALLOC / 4u + 1u); i++)
            p[i] = 0;
    }

    /* ---- Software reset -------------------------------------------------- */
    outb(g_iobase + RTL_CR, RTL_CR_RST);
    {
        uint32_t timeout = 1000000u;
        while ((inb(g_iobase + RTL_CR) & RTL_CR_RST) && --timeout)
            ;
    }

    /* ---- Unlock config registers (write-enable mode) --------------------- */
    outb(g_iobase + RTL_9346CR, 0xC0u);

    /* ---- Program DMA buffer addresses ------------------------------------ */
    outl(g_iobase + RTL_RBSTART, NET_RX_BUF_PHYS);

    {
        int i;
        for (i = 0; i < 4; i++) {
            uint32_t phys = NET_TX_BUF_PHYS + (uint32_t)i * NET_TX_BUF_STRIDE;
            outl(g_iobase + RTL_TSAD0 + (uint16_t)(i * 4), phys);
        }
    }

    /* ---- Enable TX + RX -------------------------------------------------- */
    outb(g_iobase + RTL_CR, RTL_CR_TE | RTL_CR_RE);

    /* ---- RX configuration:
     *   AB  = accept broadcast
     *   APM = accept frames matching our MAC
     *   AM  = accept multicast
     *   WRAP= ring-buffer wrap mode
     *   RBLEN=0 → 8 K + 16 ring (bits 11-12 stay 0)
     *   RXFTH=all → whole-packet FIFO threshold
     *   MXDMA=1K                                                             */
    outl(g_iobase + RTL_RCR,
         RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AM |
         RTL_RCR_WRAP | RTL_RCR_RXFTH_ALL | RTL_RCR_MXDMA_1K);

    /* ---- TX configuration: standard IFG, 1K DMA burst ------------------- */
    outl(g_iobase + RTL_TCR, RTL_TCR_IFG_STD | RTL_TCR_MXDMA_1K);

    /* ---- Interrupt mask: ROK + RER + TOK + TER + overflow ---------------- */
    outw(g_iobase + RTL_IMR,
         RTL_INT_ROK | RTL_INT_RER | RTL_INT_TOK |
         RTL_INT_TER | RTL_INT_RXOVW | RTL_INT_FOVW);

    /* ---- Lock config registers ------------------------------------------- */
    outb(g_iobase + RTL_9346CR, 0x00u);

    /* ---- Read MAC address from IDR0–IDR5 --------------------------------- */
    {
        int i;
        for (i = 0; i < 6; i++)
            g_mac[i] = inb(g_iobase + RTL_IDR0 + (uint16_t)i);
    }

    /* ---- Reset driver state ---------------------------------------------- */
    g_rx_offset = 0;
    g_tx_cur    = 0;
    g_rxq_head  = 0;
    g_rxq_tail  = 0;
    g_rtl_present = 1;

    /* ---- Unmask PIC lines (IRQ2 = cascade, g_irq = NIC's IRQ) ------------ */
    pic_unmask_irq(2);
    pic_unmask_irq(g_irq);

    kprintf("  (X) RTL8139  I/O=0x%x  IRQ=%u  MAC=%x:%x:%x:%x:%x:%x\n",
            (uint32_t)g_iobase, (uint32_t)g_irq,
            (uint32_t)g_mac[0], (uint32_t)g_mac[1],
            (uint32_t)g_mac[2], (uint32_t)g_mac[3],
            (uint32_t)g_mac[4], (uint32_t)g_mac[5]);
}

/* -------------------------------------------------------------------------- */

void rtl8139_handler(void)
{
    uint16_t isr = inw(g_iobase + RTL_ISR);

    if (!isr) return;   /* spurious */

    /* Acknowledge all interrupts immediately */
    outw(g_iobase + RTL_ISR, isr);

    if (isr & (RTL_INT_ROK | RTL_INT_RER)) {
        rtl8139_do_rx();
    }

    if (isr & RTL_INT_RXOVW) {
        /* RX ring overflowed — reset read pointer to current NIC write pos */
        g_rx_offset = (uint32_t)(inw(g_iobase + RTL_CBR)) & (NET_RX_BUF_SIZE - 1u);
        outw(g_iobase + RTL_CAPR,
             (uint16_t)((g_rx_offset - 16u) & 0xFFFFu));
    }
}

/* -------------------------------------------------------------------------- */

int rtl8139_present(void) { return g_rtl_present; }

uint8_t rtl8139_get_irq(void) { return g_irq; }

void rtl8139_get_mac(uint8_t mac[6])
{
    int i;
    for (i = 0; i < 6; i++) mac[i] = g_mac[i];
}

/* -------------------------------------------------------------------------- */

int rtl8139_send(const void *data, uint16_t len)
{
    if (!g_rtl_present || len == 0u || len > NET_MTU)
        return -1;

    /* Wait until the current TX slot is free (OWN=1 = driver owns = free).
     * After reset, all TSD registers have OWN=1.  After a TX, the NIC sets
     * OWN=1 again when it has finished sending.                              */
    {
        uint32_t timeout = 2000000u;
        while (!(inl(g_iobase + RTL_TSD0 + (uint16_t)(g_tx_cur << 2)) & RTL_TSD_OWN)
               && --timeout)
            ;
        if (!timeout) return -1;
    }

    /* Copy packet into the TX buffer for this slot */
    {
        volatile uint8_t *txbuf =
            (volatile uint8_t *)(NET_TX_BUF_PHYS +
                                 (uint32_t)g_tx_cur * NET_TX_BUF_STRIDE);
        const uint8_t *src = (const uint8_t *)data;
        uint16_t i;
        for (i = 0; i < len; i++) txbuf[i] = src[i];
    }

    /* Trigger TX: write packet size (clears OWN=bit13, handing slot to NIC) */
    outl(g_iobase + RTL_TSD0 + (uint16_t)(g_tx_cur << 2),
         (uint32_t)len & RTL_TSD_SIZE_MASK);

    g_tx_cur = (g_tx_cur + 1) & 3;
    return 0;
}

/* -------------------------------------------------------------------------- */

int rtl8139_recv(void *buf, uint16_t *out_len)
{
    if (g_rxq_head == g_rxq_tail)
        return 0;   /* queue empty */

    uint16_t len = g_rxq_len[g_rxq_head];
    uint8_t *dst = (uint8_t *)buf;
    uint16_t i;
    for (i = 0; i < len; i++) dst[i] = g_rxq_data[g_rxq_head][i];

    if (out_len) *out_len = len;
    g_rxq_head = (g_rxq_head + 1) & (NET_RXQUEUE_SLOTS - 1);
    return 1;
}
