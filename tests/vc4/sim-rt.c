/*
 * Small runtime for test programs in the resim VC4 simulator (vc4emul):
 * printf on the mini UART. SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Built with vc4-elf-gcc -fno-jump-tables: resim has no "switch"
 * instruction, so newlib's printf cannot run there.
 */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

int printf(const char *f, ...);

/* mini UART as resim's MmioHost wants it: GPIO 14 alt 5, AUX enabled */
#define REG(a) (*(volatile uint32_t *)(a))
static void sim_putc(int c)
{
    REG(0x7e215040) = (uint32_t)c;
}

static void put_num(unsigned long v, unsigned base, int width, char pad, int neg, int upper)
{
    char b[16];
    int n = 0;
    do {
        b[n++] = (upper ? "0123456789ABCDEF" : "0123456789abcdef")[v % base];
        v /= base;
    } while (v);
    if (neg)
        b[n++] = '-';
    while (width-- > n)
        sim_putc(pad);
    while (n)
        sim_putc(b[--n]);
}

int printf(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    for (; *f; f++) {
        int width = 0;
        char pad = ' ';
        if (*f != '%') {
            sim_putc(*f);
            continue;
        }
        f++;
        if (*f == '0')
            pad = '0', f++;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z')
            f++;
        if (*f == 'd' || *f == 'i') {
            long v = va_arg(ap, long);
            put_num(v < 0 ? -(unsigned long)v : (unsigned long)v, 10, width, pad, v < 0, 0);
        } else if (*f == 'u') {
            put_num(va_arg(ap, unsigned long), 10, width, pad, 0, 0);
        } else if (*f == 'x' || *f == 'X' || *f == 'p') {
            put_num(va_arg(ap, unsigned long), 16, width, pad, 0, *f == 'X');
        } else if (*f == 'c') {
            sim_putc(va_arg(ap, int));
        } else if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            int len = (int)strlen(s);
            while (width-- > len)
                sim_putc(' ');
            while (*s)
                sim_putc(*s++);
        } else {
            sim_putc('%');
            if (*f)
                sim_putc(*f);
            else
                break;
        }
    }
    va_end(ap);
    return 0;
}

void sim_uart_init(void)
{
    REG(0x7e200004) = 2u << 12; /* GPFSEL1: GPIO 14 alt 5 */
    REG(0x7e215004) = 1;        /* AUX_ENABLES: mini UART */
}
