#include "io.h"
#include "vga.h"
#include "kernel.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

/* Output routing: defaults to VGA text; replaced by gfx_putchar once
   the framebuffer is active.  kprint_redirect(NULL) reverts to VGA. */
static void vga_putchar_wrap(char c) { vga_putchar(c); }
static void (*g_putfn)(char) = vga_putchar_wrap;

void kprint_redirect(void (*fn)(char))
{
    g_putfn = fn ? fn : vga_putchar_wrap;
}

void kputs(const char *s)
{
    while (*s) g_putfn(*s++);
}

static void print_uint(uint32_t n, uint32_t base)
{
    char     buf[32];
    int      i = 0;
    const char *digits = "0123456789abcdef";
    if (n == 0) { g_putfn('0'); return; }
    while (n > 0) { buf[i++] = digits[n % base]; n /= base; }
    while (--i >= 0) g_putfn(buf[i]);
}

static void print_int(int32_t n)
{
    if (n < 0) { g_putfn('-'); print_uint((uint32_t)(-(int32_t)n), 10); }
    else         print_uint((uint32_t)n, 10);
}

void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { g_putfn(*fmt); continue; }
        fmt++;
        switch (*fmt) {
        case 'd': print_int(va_arg(args, int32_t));          break;
        case 'u': print_uint(va_arg(args, uint32_t), 10);   break;
        case 'x': print_uint(va_arg(args, uint32_t), 16);   break;
        case 'X': print_uint(va_arg(args, uint32_t), 16);   break;
        case 's': { const char *p = va_arg(args, const char *); while (*p) g_putfn(*p++); break; }
        case 'c': g_putfn((char)va_arg(args, int));          break;
        case '%': g_putfn('%');                              break;
        default:  g_putfn('%'); g_putfn(*fmt);               break;
        }
    }
    va_end(args);
}
