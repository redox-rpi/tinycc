/*
 * Speed and code generation test driver (B3s, tinycc#13/#16) for the
 * resim VC4 simulator (vc4emul). SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The work of the firmware monitor's fb command without the HDMI setup:
 * fb_pattern() draws the test pattern into a frame buffer and checks it,
 * crc32() sums the frame, at 780x438x32 and 400x300x16. fb-sim.sh takes
 * both functions from the firmware (bootcode/hvs.c, bootcode/boot64.c) and
 * builds them with vc4-tcc and with vc4-elf-gcc; this driver (built by
 * vc4-elf-gcc, printf from sim-rt.c) and a host build with FB_HOST must
 * print the same line.
 */
#include <stdint.h>
int printf(const char *f, ...);
uint32_t fb_pattern(volatile uint8_t *fb, uint32_t w, uint32_t h,
                    uint32_t depth, uint32_t order, uint32_t pitch);
uint32_t crc32(const volatile uint8_t *p, uint32_t n);

#ifdef FB_HOST
static uint8_t fb_mem[780 * 438 * 4];
#define FB ((volatile uint8_t *)fb_mem)
#else
void sim_uart_init(void);
#define FB ((volatile uint8_t *)0x00800000)
#endif

int main(void)
{
    uint32_t b1, c1, b2, c2;
#ifndef FB_HOST
    sim_uart_init();
#endif
    b1 = fb_pattern(FB, 780, 438, 32, 1, 780 * 4);
    c1 = crc32(FB, 780 * 438 * 4);
    b2 = fb_pattern(FB, 400, 300, 16, 0, 400 * 2);
    c2 = crc32(FB, 400 * 300 * 2);
    printf("fb %s %08lx %08lx\n", b1 == 0 && b2 == 0 ? "ok" : "bad",
           (unsigned long)c1, (unsigned long)c2);
    return 0;
}
