/*
 * ABI and code generation test for the VC4 backend.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * abi-sim.sh compiles this file twice: with the host compiler
 * (-funsigned-char, like the VPU) and run natively, and with vc4-tcc and
 * run in resim together with abi-main.c (vc4-elf-gcc). Both outputs must be
 * the same. Only types of the same size on both sides: no long, no
 * pointers printed.
 */
#include <stdarg.h>

int printf(const char *fmt, ...);

/* ---- variadic definitions, called from GCC code and from here ---- */

static int vsum(int n, va_list ap)
{
    int s = 0;
    while (n--)
        s = s * 3 + va_arg(ap, int);
    return s;
}

int sum(int n, ...)
{
    va_list ap;
    int s;
    va_start(ap, n);
    s = vsum(n, ap);
    va_end(ap);
    return s;
}

/* a named argument after the first one, more than six arguments */
int sum2(int a, int n, ...)
{
    va_list ap;
    int s = a;
    va_start(ap, n);
    while (n--)
        s = s * 5 + va_arg(ap, int);
    va_end(ap);
    return s;
}

/* ---- many arguments, mixed widths ---- */

int args8(int a, short b, unsigned char c, int d, int e, signed char f, int g, int h)
{
    return a * 1 + b * 3 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23;
}

static int call8(void)
{
    return args8(1, -2, 200, 4, 5, -6, 7, 8);
}

/* ---- integer operations ---- */

static unsigned ops(unsigned a, unsigned b, int x, int y)
{
    unsigned r = 0;
    r = r * 31 + (a + b);
    r = r * 31 + (a - b);
    r = r * 31 + (a * b);
    r = r * 31 + a / b;
    r = r * 31 + a % b;
    r = r * 31 + (unsigned)(x / y);
    r = r * 31 + (unsigned)(x % y);
    r = r * 31 + (a & b) + (a | b) + (a ^ b);
    r = r * 31 + (a << 5) + (a >> 3) + (unsigned)(x >> 3) + (unsigned)(x << 2);
    r = r * 31 + (a << (b & 31)) + (unsigned)(x >> (b & 31));
    r = r * 31 + ~a + (unsigned)-x;
    r = r * 31 + (a < b) + (a <= b) * 2 + (a > b) * 4 + (a >= b) * 8;
    r = r * 31 + (x < y) + (x <= y) * 2 + (x > y) * 4 + (x >= y) * 8;
    r = r * 31 + (a == b) + (a != b) * 2 + !a * 4 + (x && y) * 8 + (x || 0) * 16;
    r = r * 31 + (a + 1000) + (a - 100000) + (a * 77) + (a & 0xff00) + (a | 0x10001);
    r = r * 31 + (x < -40) + (x > 1000000) * 2 + (a == 0x12345678) * 4;
    return r;
}

/* ---- narrow types, signed and unsigned loads and stores ---- */

static signed char sc[4] = { -1, -128, 127, 5 };
static unsigned char uc[4] = { 255, 128, 127, 5 };
static short ss[3] = { -1, -32768, 1234 };
static unsigned short us[3] = { 65535, 32768, 1234 };
int gi = 42;
int *gp = &gi;               /* data relocation */

static int narrow(void)
{
    int s = 0, i;
    for (i = 0; i < 4; i++)
        s = s * 7 + sc[i] + uc[i];
    for (i = 0; i < 3; i++)
        s = s * 7 + ss[i] + us[i];
    sc[0] = (signed char)300;
    ss[0] = (short)70000;
    uc[0] = (unsigned char)-3;
    s = s * 7 + sc[0] + ss[0] + uc[0];
    *gp += 1;
    return s + gi;
}

/* ---- control flow ---- */

static int sw(int x)
{
    switch (x) {
    case 0: return 10;
    case 1: return 11;
    case 2:
    case 3: return 23;
    case 100: return 1000;
    case -5: return -50;
    default: return x * 2;
    }
}

static int loops(int n)
{
    int i, j, s = 0;
    for (i = 0; i < n; i++) {
        if (i == 3)
            continue;
        j = 0;
        do {
            s += i * j;
            if (s > 5000)
                break;
        } while (++j < i);
        while (s & 1)
            s += 3;
    }
    return s;
}

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

/* ---- function pointers, arrays, structs in memory ---- */

static int twice(int x) { return 2 * x; }
static int square(int x) { return x * x; }
static int (*const table[])(int) = { twice, square, fib };

struct big { int a, b, c; };

struct big mkbig(int x)
{
    struct big r;
    r.a = x;
    r.b = x * 2;
    r.c = x * 3;
    return r;
}

static int structs(void)
{
    struct big b = mkbig(7), arr[3];
    int i, s = 0;
    for (i = 0; i < 3; i++) {
        arr[i].a = i;
        arr[i].b = b.b + i;
        arr[i].c = table[i](i + 3);
    }
    for (i = 0; i < 3; i++)
        s = s * 13 + arr[i].a + arr[i].b + arr[i].c;
    return s + b.a + b.c;
}

static int strings(const char *p)
{
    int h = 0;
    while (*p)
        h = h * 33 + *p++;
    return h;
}

/* every call into a variable first: the order in which printf's arguments
   are evaluated is unspecified, and the host's differs from TCC's */
void run_tests(void)
{
    int a, b, c, d, e, f;
    unsigned u, v, w;

    a = sum(0); b = sum(3, 1, 2, 3); c = sum(9, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    printf("sum %d %d %d\n", a, b, c);
    a = sum2(4, 2, 7, 8); b = sum2(1, 7, 1, 2, 3, 4, 5, 6, 7);
    printf("sum2 %d %d\n", a, b);
    printf("args8 %d\n", call8());
    u = ops(1000, 7, -1001, 7); v = ops(7, 1000, 123456, -5);
    w = ops(0x12345678, 3, -40, 9);
    printf("ops %u %u %u\n", u, v, w);
    printf("narrow %d\n", narrow());
    a = sw(0); b = sw(1); c = sw(3); d = sw(100); e = sw(-5); f = sw(77);
    printf("sw %d %d %d %d %d %d\n", a, b, c, d, e, f);
    a = loops(10); b = loops(40);
    printf("loops %d %d\n", a, b);
    printf("fib %d\n", fib(15));
    printf("structs %d\n", structs());
    printf("strings %d\n", strings("hello, VPU"));
}
