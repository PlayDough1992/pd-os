/* ============================================================================
 * PD-Kernel  —  PS/2 Keyboard driver (IRQ1, port 0x60)
 * ============================================================================ */

#include "keyboard.h"
#include "pic.h"
#include "kernel.h"

#define KB_DATA_PORT  0x60
#define KB_BUF_SIZE   256

/* Scancodes for modifier keys */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPS     0x3A
#define SC_LSHIFT_R 0xAA   /* release */
#define SC_RSHIFT_R 0xB6   /* release */
#define SC_NUMLOCK  0x45

/* US QWERTY scancode set 1 -> ASCII (unshifted) */
static const char scancode_map[128] = {
     0,    0,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',  '=',
    '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0,
    'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0,  '\\',
    'z',  'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
     0,  '*',    /* right shift, numpad * */
     0,  ' ',   /* left alt, space */
     0,   0,    /* caps lock, F1 */
     0,   0,    0,   0,   0,   0,   0,   0,   0,  /* F2-F10 */
     0,   0,    /* num lock, scroll lock */
    '7', '8',  '9', '-',  /* numpad 7,8,9,- */
    '4', '5',  '6', '+',  /* numpad 4,5,6,+ */
    '1', '2',  '3',       /* numpad 1,2,3   */
    '0', '.',              /* numpad 0,.     */
     0,   0,   0,          /* unused         */
     0,   0,               /* F11, F12       */
};

/* Shifted versions of the printable keys */
static const char scancode_map_shift[128] = {
     0,    0,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',  '+',
    '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0,
    'A',  'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"',  '~',
     0,  '|',
    'Z',  'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
     0,   '*',
     0,   ' ',
     0,   0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,
     0,   0,   0,   '-',
     0,   0,   0,   '+',
     0,   0,   0,
     0,   '.',
     0,   0,   0,
     0,   0,
};

static volatile uint8_t shift_down = 0;
static volatile uint8_t caps_lock  = 0;
static volatile uint8_t num_lock   = 0;
static volatile uint8_t extended   = 0;  /* set when 0xE0 prefix received */

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
    /* Drain any stale bytes in the PS/2 output buffer (e.g. 0xAA BAT byte
     * sent by the keyboard after a system reset).  If left unread, the 8042
     * will not generate new IRQs for actual keypresses. */
    while (inb(0x64) & 0x01)
        (void)inb(KB_DATA_PORT);
    pic_unmask_irq(IRQ_KEYBOARD);
}

void keyboard_handler(void)
{
    uint8_t sc = inb(KB_DATA_PORT);

    /* Extended scancode prefix — just set flag and wait for next byte */
    if (sc == 0xE0) { extended = 1; return; }
    uint8_t is_ext = extended;
    extended = 0;

    /* --- modifier key state --- */
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) { shift_down = 1; return; }
    if (sc == SC_LSHIFT_R || sc == SC_RSHIFT_R) { shift_down = 0; return; }
    if (sc == SC_CAPS)    { caps_lock = !caps_lock; return; }
    if (sc == SC_NUMLOCK) { num_lock  = !num_lock;  return; }

    /* Ignore all other key-release events (bit 7 set) */
    if (sc & 0x80) return;

    if (sc >= 128) return;

    /* Extended arrow keys — intercept before numpad lookup */
    if (is_ext) {
        char arrow = 0;
        if      (sc == 0x48) arrow = KEY_UP;
        else if (sc == 0x50) arrow = KEY_DOWN;
        else if (sc == 0x4B) arrow = KEY_LEFT;
        else if (sc == 0x4D) arrow = KEY_RIGHT;
        else if (sc == 0x49) arrow = KEY_PGUP;
        else if (sc == 0x51) arrow = KEY_PGDN;
        if (arrow) {
            uint8_t next = (uint8_t)((kb_head + 1) % KB_BUF_SIZE);
            if (next != kb_tail) { kb_buf[kb_head] = arrow; kb_head = next; }
            return;
        }
    }

    char c;
    if (shift_down) {
        c = scancode_map_shift[sc];
    } else {
        c = scancode_map[sc];
        /* Apply caps lock to letters only */
        if (caps_lock && c >= 'a' && c <= 'z')
            c = (char)(c - 32);
    }

    /* Suppress numpad keys when Num Lock is active */
    if (num_lock && (sc == 0x37 || (sc >= 0x47 && sc <= 0x53)))
        c = 0;
    /* Numpad / sends 0xE0 0x35 — suppress when Num Lock active */
    if (num_lock && is_ext && sc == 0x35)
        c = 0;

    if (c) {
        uint8_t next = (uint8_t)((kb_head + 1) % KB_BUF_SIZE);
        if (next != kb_tail) {
            kb_buf[kb_head] = c;
            kb_head = next;
        }
    }
}

char keyboard_getchar(void)
{
    while (kb_head == kb_tail)
        __asm__ volatile ("hlt");

    char c = kb_buf[kb_tail];
    kb_tail = (uint8_t)((kb_tail + 1) % KB_BUF_SIZE);
    return c;
}

/* Non-blocking: returns 0 if no key available */
char keyboard_poll(void)
{
    if (kb_head == kb_tail) return 0;
    char c = kb_buf[kb_tail];
    kb_tail = (uint8_t)((kb_tail + 1) % KB_BUF_SIZE);
    return c;
}

/* Inject a character into the keyboard ring (called from usb_keyboard_poll) */
void keyboard_inject(char c)
{
    uint8_t next = (uint8_t)((kb_head + 1) % KB_BUF_SIZE);
    if (next != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next;
    }
}
