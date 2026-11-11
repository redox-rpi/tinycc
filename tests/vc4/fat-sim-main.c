/*
 * Functional test driver for the firmware's fat.c in the resim VC4
 * simulator (vc4emul). SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Built with vc4-elf-gcc -fno-jump-tables and newlib (vc4-sim.ld), with
 * printf from sim-rt.c. Linked with a fat.c
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

void sim_uart_init(void);

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
    sim_uart_init();
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
