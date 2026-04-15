/*
 * mouse.c — PS/2 mouse driver (IRQ12)
 *
 * The PS/2 mouse is the "auxiliary" device on the 8042 keyboard controller.
 * Initialisation sequence:
 *   1. Enable auxiliary device via controller command 0xA8.
 *   2. Enable interrupt (set bit 1 of controller config byte).
 *   3. Set mouse defaults (0xF6) + stream mode (0xF4).
 *
 * Packet format (standard 3-byte):
 *   Byte 0: [Y_OVF|X_OVF|Y_SGN|X_SGN|1|MID|RGT|LFT]
 *   Byte 1: X movement (signed)
 *   Byte 2: Y movement (signed, screen Y is *inverted*)
 *
 * The absolute position is clamped to [0, screen_width-1] x [0, screen_height-1].
 * gfx_width() / gfx_height() are consulted after gfx_init().
 */

#include "mouse.h"
#include "pic.h"
#include "kernel.h"
#include "gfx.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* 8042 ports */
#define KBC_DATA    0x60
#define KBC_CMD     0x64
#define KBC_STATUS  0x64

/* 8042 status bits */
#define KBC_STAT_OBF  0x01   /* output buffer full  */
#define KBC_STAT_IBF  0x02   /* input  buffer full  */
#define KBC_STAT_AUX  0x20   /* auxiliary data flag */

/* 8042 commands */
#define KBC_CMD_AUX_ENABLE   0xA8
#define KBC_CMD_READ_CONFIG  0x20
#define KBC_CMD_WRITE_CONFIG 0x60
#define KBC_CMD_WRITE_AUX    0xD4

/* Mouse commands */
#define MOUSE_CMD_RESET      0xFF
#define MOUSE_CMD_DEFAULTS   0xF6
#define MOUSE_CMD_STREAM     0xF4
#define MOUSE_ACK            0xFA

/* ------------------------------------------------------------------ */
static int g_mpos_x   = 0;
static int g_mpos_y   = 0;
static uint8_t g_mbtns = 0;
static int g_packet_byte = 0;
static uint8_t g_packet[3];
static mouse_callback_t g_callback = 0;

/* ------------------------------------------------------------------ */
static void kbc_wait_write(void)
{
    int t = 100000;
    while (t-- && (inb(KBC_STATUS) & KBC_STAT_IBF));
}

static void kbc_wait_read(void)
{
    int t = 100000;
    while (t-- && !(inb(KBC_STATUS) & KBC_STAT_OBF));
}

static void mouse_write(uint8_t byte)
{
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_WRITE_AUX);
    kbc_wait_write();
    outb(KBC_DATA, byte);
}

static uint8_t mouse_read(void)
{
    kbc_wait_read();
    return inb(KBC_DATA);
}

/* ------------------------------------------------------------------ */
void mouse_init(void)
{
    /* Flush output buffer */
    while (inb(KBC_STATUS) & KBC_STAT_OBF)
        inb(KBC_DATA);

    /* Enable auxiliary device */
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_AUX_ENABLE);

    /* Read and modify controller config to enable IRQ12 */
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_READ_CONFIG);
    kbc_wait_read();
    uint8_t config = inb(KBC_DATA);
    config |= 0x02;   /* enable auxiliary interrupt (IRQ12) */
    config &= 0xDF;   /* clear disable-mouse clock bit      */
    kbc_wait_write();
    outb(KBC_CMD, KBC_CMD_WRITE_CONFIG);
    kbc_wait_write();
    outb(KBC_DATA, config);

    /* Set mouse defaults + enable streaming */
    mouse_write(MOUSE_CMD_DEFAULTS);
    mouse_read();   /* ACK */
    mouse_write(MOUSE_CMD_STREAM);
    mouse_read();   /* ACK */

    /* Centre cursor */
    g_mpos_x = gfx_is_active() ? gfx_width()  / 2 : 400;
    g_mpos_y = gfx_is_active() ? gfx_height() / 2 : 300;

    /* Unmask IRQ12 on slave PIC */
    pic_unmask_irq(12);
}

/* ------------------------------------------------------------------ */
void mouse_handler(void)
{
    /* Read one byte if available */
    if (!(inb(KBC_STATUS) & KBC_STAT_OBF)) return;

    uint8_t data = inb(KBC_DATA);
    g_packet[g_packet_byte] = data;

    if (g_packet_byte == 0) {
        /* Byte 0 must have bit 3 set (always-1 bit); resync if not */
        if (!(data & 0x08)) return;
    }

    g_packet_byte++;
    if (g_packet_byte < 3) return;
    g_packet_byte = 0;

    /* Decode signed deltas */
    int dx = (int)(int8_t)g_packet[1];
    int dy = (int)(int8_t)g_packet[2];

    /* X overflow / Y overflow — discard */
    if (g_packet[0] & 0x40) dx = 0;
    if (g_packet[0] & 0x80) dy = 0;

    /* Screen Y is flipped relative to mouse Y */
    g_mpos_x += dx;
    g_mpos_y -= dy;

    /* Clamp to screen */
    int max_x = gfx_is_active() ? gfx_width()  - 1 : 799;
    int max_y = gfx_is_active() ? gfx_height() - 1 : 599;
    if (g_mpos_x < 0) g_mpos_x = 0;
    if (g_mpos_y < 0) g_mpos_y = 0;
    if (g_mpos_x > max_x) g_mpos_x = max_x;
    if (g_mpos_y > max_y) g_mpos_y = max_y;

    g_mbtns = g_packet[0] & 0x07;

    if (g_callback) g_callback(g_mpos_x, g_mpos_y, g_mbtns);
}

/* ------------------------------------------------------------------ */
void mouse_get_pos(int *x, int *y)   { *x = g_mpos_x; *y = g_mpos_y; }
uint8_t mouse_get_buttons(void)      { return g_mbtns; }
void mouse_set_callback(mouse_callback_t cb) { g_callback = cb; }
