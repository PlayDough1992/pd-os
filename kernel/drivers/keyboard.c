/* ============================================================================
 * PD-Kernel  —  PS/2 Keyboard driver (IRQ1, port 0x60)
 * ============================================================================ */

#include "keyboard.h"
#include "pic.h"
#include "kernel.h"

#define KB_DATA_PORT  0x60
#define KB_BUF_SIZE   256

/* US QWERTY scancode set 1 -> ASCII (lowercase, non-shifted) */
static const char scancode_map[128] = {
     0,   0,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0,                          /* left ctrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0,  '\\',                   /* left shift, backslash */
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
     0,  '*',                    /* right shift, keypad * */
     0,  ' ',                    /* left alt, space */
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  /* F1-F10 */
     0,   0,                    /* num lock, scroll lock */
     0,   0,   0,  '-',          /* keypad 7,8,9,- */
     0,   0,   0,  '+',          /* keypad 4,5,6,+ */
     0,   0,   0,   0,   0,      /* keypad 1,2,3,0,. */
     0,   0,   0,              /* unused */
     0,   0,                    /* F11, F12 */
};

/* Circular key buffer */
static char    kb_buf[KB_BUF_SIZE];
static uint8_t kb_head = 0;
static uint8_t kb_tail = 0;

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void keyboard_init(void)
{
    pic_unmask_irq(IRQ_KEYBOARD);
}

void keyboard_handler(void)
{
    uint8_t scancode = inb(KB_DATA_PORT);

    /* Ignore key-release events (bit 7 set) */
    if (scancode & 0x80)
        return;

    if (scancode < 128) {
        char c = scancode_map[scancode];
        if (c) {
            uint8_t next = (uint8_t)((kb_head + 1) % KB_BUF_SIZE);
            if (next != kb_tail) {    /* drop if buffer full */
                kb_buf[kb_head] = c;
                kb_head = next;
            }
        }
    }
}

char keyboard_getchar(void)
{
    while (kb_head == kb_tail)
        __asm__ volatile ("hlt"); /* wait for IRQ1 to fill buffer */

    char c = kb_buf[kb_tail];
    kb_tail = (uint8_t)((kb_tail + 1) % KB_BUF_SIZE);
    return c;
}
