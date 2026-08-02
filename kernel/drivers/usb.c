/* =============================================================================
 * PD-Kernel — USB 2.0 EHCI Host Controller + Mass Storage Class (BOT/SCSI)
 *
 * Supports one USB 2.0 high-speed mass storage device.  Makes the boot USB
 * drive writable from the kernel so PD-OS can be fully self-hosted.
 *
 * Architecture:
 *   PCI class 0x0C/0x03/0x20  → EHCI host controller
 *   EHCI async schedule        → single self-referential QH per transfer
 *   USB BOT protocol           → SCSI transparent command set
 *   SCSI READ(10)/WRITE(10)    → 512-byte sector read/write
 *
 * Only USB 2.0 high-speed devices are supported.  Full/low-speed
 * (USB 1.x) devices connected to companion UHCI controllers are skipped.
 * =============================================================================
 */

#include "usb.h"
#include "pci.h"
#include "paging.h"
#include "pit.h"
#include "io.h"
#include "kernel.h"
#include "keyboard.h"  /* for keyboard_inject() */

/* No port I/O needed — all EHCI access is via MMIO (MR/MW macros). */

/* ---- MMIO access via volatile pointer ------------------------------------ */
#define MR(a)      (*(volatile uint32_t *)(uintptr_t)(a))
#define MW(a,v)    (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))

/* =============================================================================
 * EHCI register offsets
 * =============================================================================*/

/* Capability registers (at BAR0) */
#define EHCI_CAPLENGTH  0x00u   /* 8-bit length of capability registers      */
#define EHCI_HCSPARAMS  0x04u   /* structural parameters; nports in [3:0]    */
#define EHCI_HCCPARAMS  0x08u   /* capability parameters; EECP in [15:8]     */

/* Operational registers (at BAR0 + CAPLENGTH) */
#define OP_USBCMD    0x00u
#define OP_USBSTS    0x04u
#define OP_USBINTR   0x08u
#define OP_FRINDEX   0x0Cu
#define OP_CTRLSEG   0x10u
#define OP_PERLBASE  0x14u
#define OP_ASYNCADR  0x18u
#define OP_CFGFLAG   0x40u
#define OP_PORTSC(n) (0x44u + (uint32_t)(n) * 4u)

/* USBCMD bits */
#define CMD_RS     (1u << 0)   /* Run/Stop                                    */
#define CMD_HCRST  (1u << 1)   /* Host Controller Reset                       */
#define CMD_ASE    (1u << 5)   /* Async Schedule Enable                       */
#define CMD_PSE    (1u << 4)   /* Periodic Schedule Enable                    */

/* USBSTS bits */
#define STS_HALTED (1u << 12)
#define STS_ASS    (1u << 15)  /* Async Schedule Status                       */

/* PORTSC bits */
#define PSC_CCS  (1u << 0)    /* Current Connect Status                       */
#define PSC_PED  (1u << 2)    /* Port Enabled/Disabled                        */
#define PSC_PRST (1u << 8)    /* Port Reset                                   */
#define PSC_PP   (1u << 12)   /* Port Power                                   */
#define PSC_OWNER (1u << 13)  /* Port Owner: 0=EHCI, 1=companion UHCI/OHCI   */
/* W1C bits in PORTSC (bits 1,3,5 — must be written 1 to clear, never set) */
#define PSC_W1C  ((1u<<1)|(1u<<3)|(1u<<5))

/* =============================================================================
 * qTD token bits
 * =============================================================================*/
#define QTD_ACTIVE  (1u << 7)
#define QTD_HALTED  (1u << 6)
#define QTD_DBERR   (1u << 5)
#define QTD_BABBLE  (1u << 4)
#define QTD_XACT    (1u << 3)
#define QTD_ERR     (QTD_HALTED|QTD_DBERR|QTD_BABBLE|QTD_XACT)
#define QTD_PID_OUT   (0u << 8)
#define QTD_PID_IN    (1u << 8)
#define QTD_PID_SETUP (2u << 8)
#define QTD_CERR(n)   ((uint32_t)(n) << 10)
#define QTD_IOC       (1u << 15)
#define QTD_LEN(n)    ((uint32_t)(n) << 16)
#define QTD_TOGGLE    (1u << 31)
#define QTD_TERM      (1u)       /* terminate bit for next/alt_next pointers  */

/* QH horizontal-link-pointer type field */
#define QH_TYP_QH    (1u << 1)

/* QH endpoint characteristics */
#define QH_DADDR(a)   ((uint32_t)(a) & 0x7Fu)
#define QH_EP(n)      ((uint32_t)(n) << 8)
#define QH_HS         (2u << 12)  /* High-Speed                               */
#define QH_DTC        (1u << 14)  /* Data Toggle Control from qTD             */
#define QH_HEAD       (1u << 15)  /* Head of Reclamation List                 */
#define QH_MAXPKT(n)  ((uint32_t)(n) << 16)
#define QH_MULT(n)    ((uint32_t)(n) << 30)

/* =============================================================================
 * EHCI data structures (must be 32-byte aligned; identity-mapped phys==virt)
 * =============================================================================*/
typedef struct {
    volatile uint32_t next;       /* next qTD pointer (bit 0 = terminate)    */
    volatile uint32_t alt_next;   /* alternate next qTD pointer              */
    volatile uint32_t token;      /* status, PID, length, toggle             */
    volatile uint32_t buf[5];     /* buffer page pointers (4 KB pages)       */
} __attribute__((packed)) ehci_qtd_t;

typedef struct {
    volatile uint32_t hlp;        /* horizontal link pointer                 */
    volatile uint32_t epchar;     /* endpoint characteristics                */
    volatile uint32_t epcap;      /* endpoint capabilities / mult            */
    volatile uint32_t cur_qtd;    /* current qTD pointer                     */
    /* Transfer overlay (mirrors current qTD) */
    volatile uint32_t next_qtd;
    volatile uint32_t alt_qtd;
    volatile uint32_t token;
    volatile uint32_t buf[5];
} __attribute__((packed)) ehci_qh_t;

/* =============================================================================
 * Static pools — identity-mapped so physical address == pointer value
 * =============================================================================*/
#define NUM_QTD 8
static ehci_qtd_t qtd_pool[NUM_QTD]  __attribute__((aligned(32)));
static ehci_qh_t  xfr_qh             __attribute__((aligned(32)));
static uint32_t   frame_list[1024]   __attribute__((aligned(4096)));

/* Transfer buffers: page-aligned so a 512-byte transfer never crosses a     */
/* 4 KB page boundary (each qTD buffer page holds exactly one 512-byte buf). */
static uint8_t ctrl_buf[512]    __attribute__((aligned(4096)));
static uint8_t sector_buf[512]  __attribute__((aligned(4096)));

/* USB HID keyboard: interrupt-endpoint report buffer + previous state */
static uint8_t g_kbd_report[8]  __attribute__((aligned(64)));
static uint8_t g_kbd_prev[8];

/* HID keyboard EHCI structures (live in periodic schedule) */
static ehci_qh_t  kbd_qh  __attribute__((aligned(32)));
static ehci_qtd_t kbd_qtd __attribute__((aligned(32)));

/* =============================================================================
 * USB protocol structures
 * =============================================================================*/
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
} __attribute__((packed)) usb_dev_desc_t;

typedef struct {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue;
    uint8_t  iConfiguration, bmAttributes, MaxPower;
} __attribute__((packed)) usb_cfg_desc_t;

typedef struct {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass;
    uint8_t bInterfaceProtocol, iInterface;
} __attribute__((packed)) usb_if_desc_t;

typedef struct {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_ep_desc_t;

/* USB Mass Storage Bulk-Only Transport */
typedef struct {
    uint32_t dCBWSignature;           /* 0x43425355 ("USBC")                 */
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;              /* 0x80 = IN, 0x00 = OUT               */
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;            /* CDB length (1-16)                   */
    uint8_t  CBWCB[16];              /* SCSI command descriptor block        */
} __attribute__((packed)) usb_cbw_t;

typedef struct {
    uint32_t dCSWSignature;           /* 0x53425355 ("USBS")                 */
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;              /* 0 = success                         */
} __attribute__((packed)) usb_csw_t;

/* Aligned static BOT structures for DMA */
static usb_cbw_t g_cbw __attribute__((aligned(32)));
static usb_csw_t g_csw __attribute__((aligned(16)));

/* =============================================================================
 * Module-global state
 * =============================================================================*/
static uint32_t g_capbase;      /* BAR0 — capability register base            */
static uint32_t g_opbase;       /* BAR0 + CAPLENGTH — operational reg base    */
static uint8_t  g_pbus;         /* PCI bus / dev / func of current EHCI       */
static uint8_t  g_pdev;
static uint8_t  g_pfunc;
static uint8_t  g_nports;       /* number of downstream ports                 */

/* Enumerated device */
static uint8_t  g_addr;         /* assigned USB device address (1-127)        */
static uint16_t g_ep0mps;       /* EP0 max packet size                        */
static uint8_t  g_ep_in;        /* bulk-IN endpoint number                    */
static uint8_t  g_ep_out;       /* bulk-OUT endpoint number                   */
static uint16_t g_bmps;         /* bulk endpoint max packet size              */
static uint8_t  g_in_tog;       /* data toggle for bulk IN                    */
static uint8_t  g_out_tog;      /* data toggle for bulk OUT                   */
static uint32_t g_tag;          /* CBW tag counter                            */

static uint8_t  g_msc_addr;     /* saved USB address of the MSC device        */
static uint32_t g_msc_opbase;   /* opbase of the EHCI that owns the MSC       */
static int      g_usb_ok;       /* 1 = USB mass storage device ready          */

/* USB HID keyboard state */
static uint8_t  g_kbd_addr;     /* USB address of keyboard (0 = not found)    */
static uint8_t  g_kbd_ep_in;    /* interrupt-IN endpoint number               */
static uint16_t g_kbd_mps;      /* keyboard EP max packet size (usually 8)    */
static int      g_kbd_ok;       /* 1 = keyboard enumerated and polling        */

/* =============================================================================
 * Helpers
 * =============================================================================*/

/* Operational register read/write */
static inline uint32_t op_r(uint32_t off) { return MR(g_opbase + off); }
static inline void      op_w(uint32_t off, uint32_t v) { MW(g_opbase + off, v); }

/*
 * ms_delay — wait at least 'ms' milliseconds.
 * Uses the PIT tick counter (100 Hz → 10 ms/tick) with a spin fallback
 * in case interrupts are not yet enabled.
 */
static void ms_delay(uint32_t ms)
{
    uint32_t need = (ms + 9u) / 10u;   /* convert to 100-Hz ticks, ceiling   */
    if (need == 0) need = 1;
    uint32_t t0 = pit_get_ticks();
    /* max_spin: generous spin count in case PIT ticks are not advancing.
       ~2 M iterations ≈ 1 ms on a 1 GHz CPU; errs on the side of caution.  */
    volatile uint32_t max_spin = ms * 2000000u;
    while ((pit_get_ticks() - t0 < need) && (max_spin-- > 0u)) {}
}

/*
 * qtd_set_buf — fill a qTD's 5 buffer-page pointers for a contiguous buffer.
 * buf[0] = physical start; buf[1..4] = successive 4 KB page boundaries.
 * The EHCI HC uses these when a transfer spans multiple 4 KB pages.
 */
static void qtd_set_buf(ehci_qtd_t *qtd, void *data, uint32_t len)
{
    (void)len;
    uint32_t a = (uint32_t)data;
    int i;
    qtd->buf[0] = a;
    for (i = 1; i < 5; i++) {
        a = (a & ~0xFFFu) + 0x1000u;   /* next 4 KB page boundary            */
        qtd->buf[i] = a;
    }
}

/* =============================================================================
 * EHCI BIOS → OS ownership handoff
 * =============================================================================*/
static void bios_handoff(void)
{
    uint32_t hccparams = MR(g_capbase + EHCI_HCCPARAMS);
    uint8_t  eecp      = (uint8_t)((hccparams >> 8) & 0xFF);
    if (eecp < 0x40) return;                     /* no legacy support cap     */

    uint32_t cap = pci_read32(g_pbus, g_pdev, g_pfunc, eecp);
    if ((cap & 0xFF) != 0x01) return;            /* not USB Legacy Support    */

    /* Assert OS-Owned semaphore (bit 24) */
    pci_write32(g_pbus, g_pdev, g_pfunc, eecp, cap | (1u << 24));

    /* Wait up to 2 s for BIOS-Owned semaphore (bit 16) to clear */
    {
        uint32_t t0 = pit_get_ticks();
        while (pit_get_ticks() - t0 < 200u) {   /* 200 ticks × 10 ms = 2 s  */
            if (!(pci_read32(g_pbus, g_pdev, g_pfunc, eecp) & (1u << 16)))
                break;
        }
    }

    /* Clear SMI enable bits (EECP+4) to prevent spurious SMIs */
    pci_write32(g_pbus, g_pdev, g_pfunc, eecp + 4, 0);
}

/* =============================================================================
 * Core EHCI transfer execution
 *
 * Builds a self-referential single-QH async ring, enables the schedule,
 * polls the last qTD's Active bit, then disables the schedule.
 *
 * Parameters:
 *   first      — head of qTD chain to execute
 *   last       — last qTD in the chain (the one we poll for completion)
 *   addr       — USB device address
 *   ep         — endpoint number
 *   maxpkt     — endpoint max packet size
 *   timeout_ms — abort after this many milliseconds
 *
 * Returns 0 on success, -1 on EHCI error bits, -2 on timeout.
 * =============================================================================*/
static int ehci_exec(ehci_qtd_t *first, ehci_qtd_t *last,
                     uint8_t addr, uint8_t ep, uint16_t maxpkt,
                     uint32_t timeout_ms)
{
    int i;

    /* Stop the async schedule so we can safely change ASYNCLISTADDR */
    op_w(OP_USBCMD, op_r(OP_USBCMD) & ~CMD_ASE);
    {
        uint32_t t0 = pit_get_ticks();
        while ((op_r(OP_USBSTS) & STS_ASS) && (pit_get_ticks() - t0 < 20u)) {}
    }

    /* Build the transfer Queue Head:
       - Self-referential HLP: HC traverses this one QH in a loop.
       - HEAD bit set: marks it as the reclamation head, preventing infinite
         busy-looping when the QH has no active work.
       - DTC bit set: HC uses the Data Toggle from each qTD's token field.
       - Overlay: next_qtd → first qTD, alt_qtd → terminate, token = 0 (so
         HC will advance to next_qtd to fetch and execute the first qTD). */
    xfr_qh.hlp      = (uint32_t)&xfr_qh | QH_TYP_QH;
    xfr_qh.epchar   = QH_DADDR(addr) | QH_EP(ep) | QH_HS |
                      QH_DTC | QH_HEAD | QH_MAXPKT(maxpkt);
    xfr_qh.epcap    = QH_MULT(1);
    xfr_qh.cur_qtd  = 0;
    xfr_qh.next_qtd = (uint32_t)first;
    xfr_qh.alt_qtd  = QTD_TERM;
    xfr_qh.token    = 0;
    for (i = 0; i < 5; i++) xfr_qh.buf[i] = 0;

    /* Arm the async schedule with our QH and start it */
    op_w(OP_ASYNCADR, (uint32_t)&xfr_qh);
    op_w(OP_USBCMD,   op_r(OP_USBCMD) | CMD_ASE);

    /* Poll the last qTD until Active clears (transfer done or error) */
    {
        uint32_t t0    = pit_get_ticks();
        uint32_t tlim  = (timeout_ms + 9u) / 10u;
        volatile uint32_t spin = timeout_ms * 200000u;  /* 200 K iters/ms; safe up to 21s */
        while (last->token & QTD_ACTIVE) {
            if ((pit_get_ticks() - t0 > tlim) || (spin-- == 0u)) {
                op_w(OP_USBCMD, op_r(OP_USBCMD) & ~CMD_ASE);
                return -2;   /* timeout */
            }
        }
    }

    /* Disable async schedule and wait for it to actually stop */
    op_w(OP_USBCMD, op_r(OP_USBCMD) & ~CMD_ASE);
    {
        uint32_t t0 = pit_get_ticks();
        while ((op_r(OP_USBSTS) & STS_ASS) && (pit_get_ticks() - t0 < 20u)) {}
    }

    return (last->token & QTD_ERR) ? -1 : 0;
}

/* =============================================================================
 * USB control transfer  (SETUP → [DATA] → STATUS)
 * =============================================================================*/
static int ctrl_xfr(const usb_setup_t *s, void *data, uint16_t len,
                    uint8_t dir_in)
{
    const uint8_t *sr = (const uint8_t *)s;
    int i, qi = 0, setup_qi, data_qi = -1, status_qi;

    /* Copy 8-byte SETUP packet into the front of ctrl_buf (DMA-safe) */
    for (i = 0; i < 8; i++) ctrl_buf[i] = sr[i];

    /* ---- SETUP qTD: PID=SETUP, toggle=0, 8 bytes ------------------------- */
    qtd_pool[qi].alt_next = QTD_TERM;
    qtd_pool[qi].token    = QTD_ACTIVE | QTD_PID_SETUP | QTD_CERR(3) | QTD_LEN(8);
    /* Data toggle for SETUP is always 0 (bit 31 stays clear)               */
    qtd_set_buf(&qtd_pool[qi], ctrl_buf, 8);
    setup_qi = qi++;

    /* ---- Optional DATA qTD: toggle=1 ------------------------------------- */
    if (len > 0 && data != NULL) {
        qtd_pool[qi].alt_next = QTD_TERM;
        qtd_pool[qi].token    = QTD_ACTIVE |
                                (dir_in ? QTD_PID_IN : QTD_PID_OUT) |
                                QTD_CERR(3) | QTD_LEN(len) | QTD_TOGGLE;
        qtd_set_buf(&qtd_pool[qi], data, len);
        data_qi = qi++;
    }

    /* ---- STATUS qTD: opposite direction, 0 bytes, toggle=1 --------------- */
    qtd_pool[qi].next     = QTD_TERM;
    qtd_pool[qi].alt_next = QTD_TERM;
    qtd_pool[qi].token    = QTD_ACTIVE |
                            (dir_in ? QTD_PID_OUT : QTD_PID_IN) |
                            QTD_CERR(3) | QTD_IOC | QTD_TOGGLE;
    qtd_pool[qi].buf[0]   = 0;
    for (i = 1; i < 5; i++) qtd_pool[qi].buf[i] = 0;
    status_qi = qi;

    /* ---- Chain qTDs ------------------------------------------------------- */
    if (data_qi >= 0) {
        qtd_pool[setup_qi].next = (uint32_t)&qtd_pool[data_qi];
        qtd_pool[data_qi].next  = (uint32_t)&qtd_pool[status_qi];
    } else {
        qtd_pool[setup_qi].next = (uint32_t)&qtd_pool[status_qi];
    }

    return ehci_exec(&qtd_pool[setup_qi], &qtd_pool[status_qi],
                     g_addr, 0, g_ep0mps, 400u);
}

/* =============================================================================
 * USB bulk transfers
 * =============================================================================*/
static int bulk_out(void *data, uint16_t len)
{
    ehci_qtd_t *qtd = &qtd_pool[0];
    qtd->next     = QTD_TERM;
    qtd->alt_next = QTD_TERM;
    qtd->token    = QTD_ACTIVE | QTD_PID_OUT | QTD_CERR(3) | QTD_IOC |
                    QTD_LEN(len) | (g_out_tog ? QTD_TOGGLE : 0u);
    qtd_set_buf(qtd, data, len);

    int r = ehci_exec(qtd, qtd, g_addr, g_ep_out, g_bmps, 5000u);
    if (r == 0) g_out_tog ^= 1;
    return r;
}

static int bulk_in(void *data, uint16_t len)
{
    ehci_qtd_t *qtd = &qtd_pool[0];
    qtd->next     = QTD_TERM;
    qtd->alt_next = QTD_TERM;
    qtd->token    = QTD_ACTIVE | QTD_PID_IN | QTD_CERR(3) | QTD_IOC |
                    QTD_LEN(len) | (g_in_tog ? QTD_TOGGLE : 0u);
    qtd_set_buf(qtd, data, len);

    int r = ehci_exec(qtd, qtd, g_addr, g_ep_in, g_bmps, 5000u);
    if (r == 0) g_in_tog ^= 1;
    return r;
}

/* =============================================================================
 * USB Mass Storage BOT transaction
 *
 * Sends a SCSI command via Bulk-Only Transport:
 *   1. CBW  (31 bytes, bulk OUT)
 *   2. Data (optional, direction determined by flags)
 *   3. CSW  (13 bytes, bulk IN)
 *
 * flags: 0x80 = data phase is IN (device → host), 0x00 = OUT (host → device).
 * =============================================================================*/
static int do_scsi(const uint8_t *cdb, uint8_t cdb_len,
                   void *data, uint32_t data_len, uint8_t flags)
{
    int i;
    g_cbw.dCBWSignature          = 0x43425355u;
    g_cbw.dCBWTag                = ++g_tag;
    g_cbw.dCBWDataTransferLength = data_len;
    g_cbw.bmCBWFlags             = flags;
    g_cbw.bCBWLUN                = 0;
    g_cbw.bCBWCBLength           = cdb_len;
    for (i = 0; i < 16; i++)
        g_cbw.CBWCB[i] = (i < (int)cdb_len) ? cdb[i] : 0;

    if (bulk_out(&g_cbw, 31) != 0) return -1;

    if (data_len > 0) {
        if (flags & 0x80) {
            if (bulk_in (data, (uint16_t)data_len) != 0) return -1;
        } else {
            if (bulk_out(data, (uint16_t)data_len) != 0) return -1;
        }
    }

    if (bulk_in(&g_csw, 13) != 0)               return -1;
    if (g_csw.dCSWSignature != 0x53425355u)      return -1;
    if (g_csw.dCSWTag       != g_tag)            return -1;
    if (g_csw.bCSWStatus    != 0)                return -1;
    return 0;
}

/* =============================================================================
 * HID boot-protocol keycode → ASCII lookup tables
 * =============================================================================*/
static const char hid_normal[0x60] = {
    0,   0,   0,   0,
    'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n','\x1B','\b','\t',' ','-','=','[',']','\\',0,';','\'','`',',','.','/',
    0,                       /* 0x39 CapsLock */
    0,0,0,0,0,0,0,0,0,0,0,0,/* 0x3A-0x45 F1-F12 */
    0,0,0,0,0,0,0,0,         /* 0x46-0x4D PrtScr..End */
    0,                       /* 0x4E PgDn */
    '\x14','\x13','\x12','\x11', /* 0x4F-0x52 R/L/D/U arrows */
    0,0,0,0,0,0,0,0,0,0,0    /* 0x53-0x5D numpad */
};
static const char hid_shift[0x60] = {
    0,   0,   0,   0,
    'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n','\x1B','\b','\t',' ','_','+','{','}','|',0,':','"','~','<','>','?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,
    '\x14','\x13','\x12','\x11',
    0,0,0,0,0,0,0,0,0,0,0
};

static char hid_to_ascii(uint8_t hid, uint8_t shift)
{
    if (hid >= 0x60) return 0;
    return shift ? hid_shift[hid] : hid_normal[hid];
}

/* =============================================================================
 * USB device enumeration (called after port reset)
 *
 * Tries to identify the device as either a Mass Storage device (MSC) or a
 * USB HID boot-protocol keyboard, assigns it new_addr, and configures it.
 *
 * Returns:
 *   1  — MSC device enumerated; g_addr/g_ep_in/g_ep_out/g_bmps filled
 *   2  — HID keyboard enumerated; g_kbd_addr/g_kbd_ep_in/g_kbd_mps filled
 *   0  — Device present but type unknown (address was assigned)
 *  -1  — Enumeration failed
 * =============================================================================*/
static int enumerate_device(uint8_t new_addr)
{
    uint8_t  *p, *end;
    uint16_t  total;
    uint8_t   cfgval;
    int       in_msc = 0, in_hid_kbd = 0;
    uint8_t   kbd_ep = 0, kbd_iface = 0;
    uint16_t  kbd_mps = 8;

    /* All control transfers start at address 0 (device default) */
    g_addr   = 0;
    g_ep0mps = 64;

    /* GET_DESCRIPTOR (Device, 8 bytes) — learn EP0 max-packet-size */
    {
        usb_setup_t s = { 0x80, 0x06, 0x0100, 0, 8 };
        if (ctrl_xfr(&s, ctrl_buf, 8, 1) != 0) return -1;
        g_ep0mps = ((usb_dev_desc_t *)ctrl_buf)->bMaxPacketSize0;
        if (!g_ep0mps) g_ep0mps = 64;
    }

    /* SET_ADDRESS new_addr */
    {
        usb_setup_t s = { 0x00, 0x05, new_addr, 0, 0 };
        if (ctrl_xfr(&s, NULL, 0, 0) != 0) return -1;
    }
    ms_delay(5);
    g_addr = new_addr;

    /* GET_DESCRIPTOR (Configuration, 9 bytes) */
    {
        usb_setup_t s = { 0x80, 0x06, 0x0200, 0, 9 };
        if (ctrl_xfr(&s, ctrl_buf, 9, 1) != 0) return 0;  /* addr assigned, give up */
    }
    total  = ((usb_cfg_desc_t *)ctrl_buf)->wTotalLength;
    cfgval = ((usb_cfg_desc_t *)ctrl_buf)->bConfigurationValue;
    if (total > (uint16_t)sizeof(ctrl_buf)) total = (uint16_t)sizeof(ctrl_buf);

    /* GET_DESCRIPTOR (Configuration, full blob) */
    {
        usb_setup_t s = { 0x80, 0x06, 0x0200, 0, total };
        if (ctrl_xfr(&s, ctrl_buf, total, 1) != 0) return 0;
    }

    /* Parse descriptor blob — look for MSC and/or HID keyboard interfaces */
    g_ep_in = g_ep_out = 0;
    g_bmps  = 512;
    p   = ctrl_buf;
    end = p + total;

    while (p < end && p[0] >= 2u) {
        if (p[1] == 0x04) {                             /* Interface descriptor */
            usb_if_desc_t *ifd = (usb_if_desc_t *)p;
            in_msc     = (ifd->bInterfaceClass   == 0x08 &&
                          ifd->bInterfaceSubClass == 0x06 &&
                          ifd->bInterfaceProtocol == 0x50);
            in_hid_kbd = (!g_ep_in &&               /* only if no MSC yet    */
                          ifd->bInterfaceClass   == 0x03 &&
                          ifd->bInterfaceSubClass == 0x01 &&
                          ifd->bInterfaceProtocol == 0x01);
            if (in_hid_kbd) kbd_iface = ifd->bInterfaceNumber;
        } else if (p[1] == 0x05) {                      /* Endpoint descriptor  */
            usb_ep_desc_t *epd = (usb_ep_desc_t *)p;
            if (in_msc && (epd->bmAttributes & 0x03) == 0x02) {
                if (epd->bEndpointAddress & 0x80)
                    g_ep_in  = epd->bEndpointAddress & 0x0F;
                else
                    g_ep_out = epd->bEndpointAddress & 0x0F;
                g_bmps = epd->wMaxPacketSize;
            }
            if (in_hid_kbd && (epd->bmAttributes & 0x03) == 0x03 &&
                (epd->bEndpointAddress & 0x80)) {        /* Interrupt IN         */
                kbd_ep  = epd->bEndpointAddress & 0x0F;
                kbd_mps = epd->wMaxPacketSize;
            }
        }
        p += p[0];
    }

    /* ---- Mass Storage device -------------------------------------------- */
    if (g_ep_in && g_ep_out) {
        usb_setup_t sc = { 0x00, 0x09, cfgval, 0, 0 };
        if (ctrl_xfr(&sc, NULL, 0, 0) != 0) return -1;

        { usb_setup_t mr = { 0x21, 0xFF, 0, 0, 0 }; ctrl_xfr(&mr, NULL, 0, 0); }
        g_in_tog = g_out_tog = 0;

        {
            uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
            if (do_scsi(cdb, 6, ctrl_buf, 36, 0x80) != 0) return -1;
            if ((ctrl_buf[0] & 0x1F) != 0x00) return -1;  /* not a disk */
        }
        return 1;
    }

    /* ---- HID boot-protocol keyboard ------------------------------------- */
    if (kbd_ep) {
        usb_setup_t sc  = { 0x00, 0x09, cfgval, 0, 0 };
        usb_setup_t sp  = { 0x21, 0x0B, 0x0000, kbd_iface, 0 };  /* SET_PROTOCOL boot */
        usb_setup_t si  = { 0x21, 0x0A, 0x0000, kbd_iface, 0 };  /* SET_IDLE 0ms      */
        ctrl_xfr(&sc, NULL, 0, 0);
        ctrl_xfr(&sp, NULL, 0, 0);
        ctrl_xfr(&si, NULL, 0, 0);  /* ignore results — device may NAK SET_IDLE */

        g_kbd_addr  = new_addr;
        g_kbd_ep_in = kbd_ep;
        g_kbd_mps   = kbd_mps ? kbd_mps : 8;
        return 2;
    }

    return 0;  /* device present, type unknown */
}

/* =============================================================================
 * kbd_setup_periodic — arm the keyboard QH in the EHCI periodic schedule
 * =============================================================================*/
static void kbd_setup_periodic(void)
{
    int j;

    /* Build the keyboard interrupt-IN qTD */
    kbd_qtd.next     = QTD_TERM;
    kbd_qtd.alt_next = QTD_TERM;
    kbd_qtd.token    = QTD_ACTIVE | QTD_PID_IN | QTD_CERR(3) | QTD_IOC |
                       QTD_LEN(8);
    kbd_qtd.buf[0]   = (uint32_t)g_kbd_report;
    kbd_qtd.buf[1]   = (kbd_qtd.buf[0] & ~0xFFFu) + 0x1000u;
    for (j = 2; j < 5; j++) kbd_qtd.buf[j] = kbd_qtd.buf[j-1] + 0x1000u;

    /* Build the keyboard QH
       Speed = HS (2), DTC = 0 (toggle tracked by HC in overlay),
       S-mask = 0x01 (poll at microframe 0 of every 1 ms frame), mult = 1 */
    kbd_qh.hlp      = QTD_TERM;               /* only QH in periodic ring    */
    kbd_qh.epchar   = QH_DADDR(g_kbd_addr) | QH_EP(g_kbd_ep_in) | QH_HS |
                      QH_MAXPKT(g_kbd_mps);
    kbd_qh.epcap    = QH_MULT(1) | 0x01u;     /* S-mask = microframe 0       */
    kbd_qh.cur_qtd  = 0;
    kbd_qh.next_qtd = (uint32_t)&kbd_qtd;
    kbd_qh.alt_qtd  = QTD_TERM;
    kbd_qh.token    = 0;
    for (j = 0; j < 5; j++) kbd_qh.buf[j] = 0;

    /* Point every frame-list entry at the keyboard QH */
    for (j = 0; j < 1024; j++)
        frame_list[j] = (uint32_t)&kbd_qh | QH_TYP_QH;

    /* Enable the periodic schedule */
    op_w(OP_USBCMD, op_r(OP_USBCMD) | CMD_PSE);
}

/* =============================================================================
 * Port enumeration — reset each HS port and identify MSC / HID keyboard
 * =============================================================================*/
static int probe_ports(void)
{
    uint32_t i;
    uint8_t  next_addr = 1;

    for (i = 0; i < (uint32_t)g_nports; i++) {
        uint32_t psc = op_r(OP_PORTSC(i));
        if (!(psc & PSC_CCS)) continue;

        op_w(OP_PORTSC(i), (psc & ~PSC_W1C) | PSC_PRST);
        ms_delay(55);
        op_w(OP_PORTSC(i), op_r(OP_PORTSC(i)) & ~(PSC_W1C | PSC_PRST));
        ms_delay(20);

        psc = op_r(OP_PORTSC(i));
        if (!(psc & PSC_PED)) {
            /* FS/LS device — release port to companion UHCI controller      */
            op_w(OP_PORTSC(i), (psc & ~PSC_W1C) | PSC_OWNER);
            kprintf("  (0) USB: port %u FS/LS, released to companion\n", i);
            continue;
        }

        kprintf("  (0) USB: port %u HS device\n", i);
        {
            int type = enumerate_device(next_addr);
            next_addr++;  /* address was assigned (or attempted) */

            if (type == 1 && !g_usb_ok) {
                g_msc_addr  = g_addr;            /* save for disk I/O        */
                g_msc_opbase = g_opbase;         /* save EHCI opbase         */
                g_usb_ok    = 1;
                kprintf("  (X) USB: MSC addr=%u EP IN=%u OUT=%u\n",
                        g_msc_addr, g_ep_in, g_ep_out);
            } else if (type == 2 && !g_kbd_ok) {
                /* keyboard: g_kbd_addr/ep_in/mps already set               */
                kbd_setup_periodic();
                g_kbd_ok = 1;
                kprintf("  (X) USB: keyboard addr=%u EP=%u\n",
                        g_kbd_addr, g_kbd_ep_in);
            } else if (type <= 0) {
                kprintf("  (!) USB: port %u: unknown/error (%d)\n", i, type);
            }
        }
    }

    /* Restore g_addr for MSC disk I/O */
    if (g_usb_ok) g_addr = g_msc_addr;

    return g_usb_ok ? 0 : -1;
}

/* =============================================================================
 * EHCI host controller initialisation
 * =============================================================================*/
static int ehci_hc_init(uint32_t bar0)
{
    int j;
    g_capbase = bar0;
    g_opbase  = bar0 + (uint8_t)(MR(g_capbase + EHCI_CAPLENGTH) & 0xFF);
    g_nports  = (uint8_t)(MR(g_capbase + EHCI_HCSPARAMS) & 0x0Fu);

    bios_handoff();

    /* Stop HC if currently running */
    op_w(OP_USBCMD, op_r(OP_USBCMD) & ~CMD_RS);
    {
        uint32_t t0 = pit_get_ticks();
        while (!(op_r(OP_USBSTS) & STS_HALTED) && (pit_get_ticks() - t0 < 30u)) {}
    }

    /* Full HC reset */
    op_w(OP_USBCMD, CMD_HCRST);
    {
        uint32_t t0 = pit_get_ticks();
        while ((op_r(OP_USBCMD) & CMD_HCRST) && (pit_get_ticks() - t0 < 100u)) {}
    }
    if (op_r(OP_USBCMD) & CMD_HCRST) return -1;   /* reset timed out         */

    /* Initialise periodic frame list (all entries = terminate) */
    for (j = 0; j < 1024; j++) frame_list[j] = QTD_TERM;

    /* Configure HC */
    op_w(OP_CTRLSEG,  0);                          /* 32-bit mode             */
    op_w(OP_PERLBASE, (uint32_t)frame_list);        /* periodic list base      */
    op_w(OP_USBINTR,  0);                           /* no interrupts           */

    /* Start HC (frame list size = 1024, periodic schedule disabled initially) */
    op_w(OP_USBCMD, CMD_RS);
    {
        uint32_t t0 = pit_get_ticks();
        while ((op_r(OP_USBSTS) & STS_HALTED) && (pit_get_ticks() - t0 < 30u)) {}
    }
    if (op_r(OP_USBSTS) & STS_HALTED) return -1;   /* HC failed to start      */

    /* Route all downstream ports to EHCI (not companion UHCI/OHCI) */
    op_w(OP_CFGFLAG, 1);
    ms_delay(10);

    return 0;
}

/* =============================================================================
 * Public API
 * =============================================================================*/

int usb_init(void)
{
    uint8_t bus, dev, func;

    g_usb_ok = 0;
    bus = dev = func = 0;

    /* Scan for all EHCI controllers (PCI class 0x0C / subclass 0x03 / PI 0x20) */
    while (pci_find_class(0x0C, 0x03, 0x20, &bus, &dev, &func)) {
        g_pbus = bus; g_pdev = dev; g_pfunc = func;

        /* Read BAR0 — must be a 32-bit memory BAR */
        uint32_t bar0 = pci_read32(bus, dev, func, 0x10);
        if (bar0 & 0x01u) goto next;           /* I/O BAR, not expected       */
        bar0 &= ~0x0Fu;                         /* strip flag bits             */
        if (!bar0) goto next;

        kprintf("  (0) USB: EHCI PCI %02x:%02x.%x BAR0=0x%08x\n",
                bus, dev, func, bar0);

        /* Map EHCI MMIO into the kernel's virtual address space.
           paging_map_framebuffer rounds down to a 4 MB boundary and installs
           a PSE PDE — identical to how the network driver maps its DMA pages. */
        paging_map_framebuffer(bar0);

        /* Enable PCI Memory Space + Bus Mastering */
        {
            uint32_t cmd = pci_read32(bus, dev, func, 0x04);
            pci_write32(bus, dev, func, 0x04, cmd | 0x06u);
        }

        if (ehci_hc_init(bar0) != 0) {
            kprintf("  (!) USB: HC init failed\n");
            goto next;
        }

        /* Process ALL ports on this controller before moving to the next.
           FS/LS ports get PSC_OWNER=1 here so UHCI sees them later.   */
        probe_ports();

    next:
        if (++func >= 8u) { func = 0; if (++dev >= 32u) { dev = 0; bus++; } }
        if (bus >= 255u) break;
    }

    /* Restore opbase/addr of the EHCI that owns the MSC device         */
    if (g_usb_ok) {
        g_opbase = g_msc_opbase;
        g_addr   = g_msc_addr;
        kprintf("  (X) USB: MSC ready (EP IN=%u OUT=%u MPS=%u)\n",
                g_ep_in, g_ep_out, g_bmps);
    } else {
        kprintf("  (!) USB: no MSC device found\n");
    }

    return g_usb_ok ? 0 : -1;
}

int usb_available(void)
{
    return g_usb_ok;
}

int usb_read_sector(uint32_t lba, void *buf)
{
    uint8_t cdb[10];
    uint8_t *dst;
    int i, r;

    cdb[0] = 0x28;                              /* READ(10)                   */
    cdb[1] = 0;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >>  8);
    cdb[5] = (uint8_t)(lba);
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;                                 /* transfer length = 1 block  */
    cdb[9] = 0;

    r = do_scsi(cdb, 10, sector_buf, 512, 0x80);
    if (r != 0) return -1;

    dst = (uint8_t *)buf;
    for (i = 0; i < 512; i++) dst[i] = sector_buf[i];
    return 0;
}

int usb_write_sector(uint32_t lba, const void *buf)
{
    uint8_t cdb[10];
    const uint8_t *src;
    int i;

    src = (const uint8_t *)buf;
    for (i = 0; i < 512; i++) sector_buf[i] = src[i];

    cdb[0] = 0x2A;                              /* WRITE(10)                  */
    cdb[1] = 0;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >>  8);
    cdb[5] = (uint8_t)(lba);
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;                                 /* transfer length = 1 block  */
    cdb[9] = 0;

    return do_scsi(cdb, 10, sector_buf, 512, 0x00);
}

/* =============================================================================
 * usb_keyboard_poll — inject any newly-pressed keys into the keyboard ring
 *
 * Called from keyboard_getchar() inside the hlt loop and from keyboard_poll().
 * Safe to call at any time; returns immediately if no keyboard or no new data.
 * =============================================================================*/
void usb_keyboard_poll(void)
{
    int     i, j;
    uint8_t mod, shift, key;
    char    c;

    /* EHCI keyboard (HS device enumerated directly on EHCI) */
    if (!g_kbd_ok) return;
    if (kbd_qtd.token & QTD_ACTIVE) return;     /* transfer still pending     */

    /* Clear error and resubmit — keyboard will NAK occasionally */
    if (kbd_qtd.token & QTD_ERR) {
        uint32_t t = kbd_qh.token & QTD_TOGGLE;
        kbd_qtd.token   = QTD_ACTIVE | QTD_PID_IN | QTD_CERR(3) | QTD_IOC | QTD_LEN(8);
        kbd_qh.next_qtd = (uint32_t)&kbd_qtd;
        kbd_qh.alt_qtd  = QTD_TERM;
        kbd_qh.token    = t;
        return;
    }

    mod   = g_kbd_report[0];
    shift = ((mod & 0x02u) || (mod & 0x20u)) ? 1 : 0;

    /* Inject every key that is newly pressed (not present in previous report) */
    for (i = 2; i < 8; i++) {
        key = g_kbd_report[i];
        if (key == 0) continue;
        for (j = 2; j < 8; j++) {
            if (g_kbd_prev[j] == key) goto held;
        }
        c = hid_to_ascii(key, shift);
        if (c) keyboard_inject(c);
    held:;
    }

    /* Save current report as previous */
    for (i = 0; i < 8; i++) g_kbd_prev[i] = g_kbd_report[i];

    /* Resubmit qTD — preserve data toggle stored in QH overlay */
    {
        uint32_t t = kbd_qh.token & QTD_TOGGLE;
        kbd_qtd.token   = QTD_ACTIVE | QTD_PID_IN | QTD_CERR(3) | QTD_IOC | QTD_LEN(8);
        kbd_qh.next_qtd = (uint32_t)&kbd_qtd;
        kbd_qh.alt_qtd  = QTD_TERM;
        kbd_qh.token    = t;
    }
}
