/*
 * Functional test driver for the firmware's fat.c in the resim VC4
 * simulator (vc4emul). SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Built with vc4-elf-gcc -fno-jump-tables and newlib (vc4-sim.ld): resim
 * has no "switch" instruction, so newlib's printf cannot run there; this
 * file brings its own small printf on the mini UART. Linked with a fat.c
 * object from vc4-tcc or from vc4-elf-gcc. The card image sits in the
 * simulator's memory at CARD (fat-sim.sh puts it there); files are loaded to
 * LOAD and a checksum of each is printed, one line per name:
 *     <name> <size> <fnv1a-32 hex>      or     <name> missing
 */
#include <string.h>
#include "hw.h"

#define CARD 0x01000000u
#define LOAD 0x05000000u
#define LOAD_MAX 0x01000000u

bool fat_list(void);
int32_t fat_load(const char *name, uint32_t addr, uint32_t max);

struct sd_card g_sd;

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

bool sd_init(void)
{
    g_sd.ready = true;
    return true;
}

bool sd_read(uint32_t lba, uint32_t *buf)
{
    memcpy(buf, (const void *)(CARD + lba * 512u), 512);
    return true;
}

static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static const char *const names[] = {
    "big-file.data", "armstub8-redox.bin", "U-BOOT.BIN", "CONFIG.TXT",
    "no-such-file.txt",
};

int main(void)
{
    unsigned i;
    REG(0x7e200004) = 2u << 12; /* GPFSEL1: GPIO 14 alt 5 */
    REG(0x7e215004) = 1;        /* AUX_ENABLES: mini UART */
    printf("fat-sim: start\n");
    fat_list();
    for (i = 0; i < sizeof names / sizeof *names; i++) {
        int32_t n = fat_load(names[i], LOAD, LOAD_MAX);
        if (n < 0)
            printf("RESULT %s missing\n", names[i]);
        else
            printf("RESULT %s %ld %08lx\n", names[i], (long)n,
                   (unsigned long)fnv1a((const uint8_t *)LOAD, (uint32_t)n));
    }
    printf("fat-sim: end\n");
    return 0;
}
