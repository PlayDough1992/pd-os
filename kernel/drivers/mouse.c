/* ============================================================================
 * PD-OS GDE  —  PS/2 mouse driver (8042 auxiliary device, IRQ12)
 * ============================================================================ */

#include "mouse.h"
#include "pic.h"

#define SCREEN_W 1024
#define SCREEN_H  768

/* 8042 ports */
#define PS2_DATA    0x60
#define PS2_CMD     0x64
#define PS2_STATUS  0x64

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- State --------------------------------------------------------------- */

static int     g_mx       = SCREEN_W / 2;
static int     g_my       = SCREEN_H / 2;
static uint8_t g_buttons  = 0;
static int     g_changed  = 0;
/* Press latch: bits set when a button transitions 0→1 in the ISR.
 * Never cleared by the ISR — only by mouse_clear_btn_latch().
 * Lets the event loop detect presses that completed (down+up) between
 * two loop iterations, which plain `buttons & ~prev` would miss. */
static uint8_t g_btn_latch = 0;

/* 3-byte packet accumulator */
static uint8_t g_buf[3];
static int     g_phase = 0;

/* ---- 8042 helpers -------------------------------------------------------- */

static void ps2_wait_write(void)
{
    int timeout = 100000;
    while (timeout-- > 0 && (inb(PS2_STATUS) & 0x02)) /* IBF set = busy */ ;
}

static void ps2_wait_read(void)
{
    int timeout = 100000;
    while (timeout-- > 0 && !(inb(PS2_STATUS) & 0x01)) /* OBF clear = empty */;
}

/* Send a byte directly to the PS/2 data port (port 0x60) */
static void ps2_write_data(uint8_t b)
{
    ps2_wait_write();
    outb(PS2_DATA, b);
}

/* Route next write to the AUX (mouse) device */
static void ps2_write_mouse(uint8_t b)
{
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);   /* "send to AUX" */
    ps2_wait_write();
    outb(PS2_DATA, b);
}

/* Read one byte from the data port (discards byte) */
static uint8_t ps2_read_data(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* ---- Init ---------------------------------------------------------------- */

void mouse_init(void)
{
    /* 1. Enable AUX device */
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);

    /* 2. Read current config byte, enable AUX IRQ (bit 1), clear AUX disable (bit 5) */
    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    uint8_t cfg = ps2_read_data();
    cfg |=  (1u << 1);   /* enable IRQ12 */
    cfg &= ~(1u << 5);   /* clear AUX clock disable */
    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_write_data(cfg);

    /* 3. Set mouse defaults */
    ps2_write_mouse(0xF6);
    ps2_read_data();  /* ACK */

    /* 4. Enable mouse streaming */
    ps2_write_mouse(0xF4);
    ps2_read_data();  /* ACK */

    /* 5. Unmask IRQ12 so we receive mouse packets.
     *    Also unmask IRQ2 (master PIC cascade line) — required for any
     *    slave PIC interrupt (IRQ8-15) to reach the CPU. */
    pic_unmask_irq(2);
    pic_unmask_irq(12);
}

/* ---- IRQ12 handler ------------------------------------------------------- */

void mouse_handler(void)
{
    uint8_t data = inb(PS2_DATA);

    /* Synchronise: first byte of a packet must have bit 3 set */
    if (g_phase == 0 && !(data & 0x08)) return;

    g_buf[g_phase++] = data;
    if (g_phase < 3) return;
    g_phase = 0;

    /* Decode the 3-byte packet */
    uint8_t flags = g_buf[0];
    int     dx    = (int)(int8_t)g_buf[1];
    int     dy    = (int)(int8_t)g_buf[2];

    /* Ignore overflow packets */
    if (flags & 0xC0) return;

    /* Update absolute position (PS/2 Y is inverted — up = positive) */
    g_mx += dx;
    g_my -= dy;

    if (g_mx < 0)         g_mx = 0;
    if (g_mx >= SCREEN_W) g_mx = SCREEN_W - 1;
    if (g_my < 0)         g_my = 0;
    if (g_my >= SCREEN_H) g_my = SCREEN_H - 1;

    uint8_t new_buttons  = flags & 0x07u;
    uint8_t newly_pressed = (uint8_t)(~g_buttons & new_buttons); /* 0→1 bits */
    g_btn_latch |= newly_pressed;
    if (new_buttons != g_buttons || dx || dy) g_changed = 1;
    g_buttons = new_buttons;
}

/* ---- Accessors ----------------------------------------------------------- */

int     mouse_get_x(void)       { return g_mx; }
int     mouse_get_y(void)       { return g_my; }
uint8_t mouse_get_buttons(void) { return g_buttons; }

int mouse_changed(void)
{
    return g_changed;
}

void mouse_clear_changed(void)
{
    g_changed = 0;
}

uint8_t mouse_get_btn_latch(void)   { return g_btn_latch; }
void    mouse_clear_btn_latch(void) { g_btn_latch = 0; }
