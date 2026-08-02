/* ============================================================================
 * PD-Kernel  —  Formatted kernel output (kprintf / kputs)
 * ============================================================================ */

#include "io.h"
#include "vga.h"
#include "kernel.h"

/* Use GCC built-in variadic support (no <stdarg.h> needed) */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

void kputs(const char *s)
{
    vga_puts(s);
}

static void print_uint(uint32_t n, uint32_t base)
{
    char     buf[32];
    int      i = 0;
    const char *digits = "0123456789abcdef";

    if (n == 0) {
        vga_putchar('0');
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % base];
        n /= base;
    }
    while (--i >= 0)
        vga_putchar(buf[i]);
}

static void print_int(int32_t n)
{
    if (n < 0) {
        vga_putchar('-');
        print_uint((uint32_t)(-(int32_t)n), 10);
    } else {
        print_uint((uint32_t)n, 10);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            vga_putchar(*fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': print_int(va_arg(args, int32_t));         break;
        case 'u': print_uint(va_arg(args, uint32_t), 10);  break;
        case 'x': print_uint(va_arg(args, uint32_t), 16);  break;
        case 'X': print_uint(va_arg(args, uint32_t), 16);  break;
        case 's': vga_puts(va_arg(args, const char *));     break;
        case 'c': vga_putchar((char)va_arg(args, int));     break;
        case '%': vga_putchar('%');                         break;
        default:  vga_putchar('%'); vga_putchar(*fmt);      break;
        }
    }

    va_end(args);
}
