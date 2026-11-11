/*
 * Driver for abi-test.c in the resim simulator (built with vc4-elf-gcc).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
void sim_uart_init(void);
void run_tests(void);
int printf(const char *fmt, ...);
int sum(int n, ...);

int main(void)
{
    sim_uart_init();
    run_tests();
    /* a variadic TCC function called from GCC code */
    printf("gcc-calls-sum %d\n", sum(7, 7, 6, 5, 4, 3, 2, 1));
    return 0;
}
