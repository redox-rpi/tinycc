/*
 * Encoder test for vc4-enc.h (SPDX-License-Identifier: LGPL-2.1-or-later).
 *
 * Emits every instruction form the VC4 backend uses. Writes the raw code to
 * argv[1] and one line per instruction to stdout:
 *     <offset hex> TAB <expected objdump text> TAB <gas: 1 = vc4-elf-as must
 *     give the same bytes for the text, 0 = objdump check only>
 * enc-test.py compares both against the vc4 binutils.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

static uint8_t buf[65536];
static int pos;
#define VC4_EMIT16(h) (buf[pos] = (h) & 0xff, buf[pos + 1] = ((h) >> 8) & 0xff, pos += 2)
#include "../../vc4-enc.h"

static int start;
static void begin(void) { start = pos; }
static void expect(int gas, const char *fmt, ...)
{
    va_list ap;
    printf("%x\t", start);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\t%d\n", gas);
}
#define T(gas, call, ...) do { begin(); call; expect(gas, __VA_ARGS__); } while (0)

static const char *const alu_name[32] = {
    "mov", "cmn", "add", "bic", "mul", "eor", "sub", "and",
    "not", "ror", "cmp", "rsub", "btest", "or", "bmask", "max",
    "bitset", "min", "bitclear", NULL, "bitflip", NULL, NULL, NULL,
    "signext", "neg", "lsr", "msb", "shl", "brev", "asr", "abs"
};
static const char *const cc_name[16] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "", "f"
};
static const char *const sz_name[8] = {
    "ld", "st", "ldh", "sth", "ldb", "stb", "ldsh", "ldsb"
};
static const char *reg(int r)
{
    static char b[4][8];
    static int i;
    i = (i + 1) & 3;
    if (r == 24) return "gp";
    if (r == 25) return "sp";
    if (r == 26) return "lr";
    if (r == 30) return "sr";
    if (r == 31) return "pc";
    snprintf(b[i], 8, "r%d", r);
    return b[i];
}

int main(int argc, char **argv)
{
    int op, i, cc, sz;
    static const int ops32[] = { VC4_ADD, VC4_SUB, VC4_MUL, VC4_AND,
        VC4_OR, VC4_EOR, VC4_SHL, VC4_LSR, VC4_ASR, VC4_BIC };
    static const int imm5ops[] = { VC4_MOV, VC4_ADD, VC4_SUB, VC4_MUL,
        VC4_CMP, VC4_SHL, VC4_LSR, VC4_ASR };
    static const int offs12[] = { 64, 2000, 2047, -4, -100, -2048 };
    static const int offs27[] = { 4096, -4096, 67108863, -67108864 };

    T(1, vc4_nop(), "nop");
    T(1, vc4_rts(), "rts");
    for (i = 0; i < 16; i += 5) {
        T(1, vc4_b_reg(i), "b %s", reg(i));
        T(1, vc4_bl_reg(i), "bl %s", reg(i));
    }
    T(1, vc4_push_r6_r7_lr(), "stm r6-r7,lr,(--sp)");
    T(1, vc4_pop_r6_r7_pc(), "ldm r6-r7,pc,(sp++)");

    /* 16-bit ALU, register form */
    for (op = 0; op < 32; op++) {
        if (!alu_name[op])
            continue;
        T(1, vc4_alu16(op, 3, 12), "%s r3,r12", alu_name[op]);
    }
    T(1, vc4_alu16(VC4_ADD, 15, 0), "add r15,r0");

    /* 16-bit ALU, 5-bit immediate */
    for (i = 0; i < (int)(sizeof imm5ops / sizeof *imm5ops); i++) {
        op = imm5ops[i];
        T(1, vc4_alu16i(op, 5, 31), "%s r5,0x1f", alu_name[op]);
        T(1, vc4_alu16i(op, 0, 1), "%s r0,0x1", alu_name[op]);
    }

    /* 16-bit loads and stores */
    for (sz = 0; sz < 8; sz++)
        T(1, vc4_ldst16(sz, 1, 14), "%s r1,(r14)", sz_name[sz]);
    T(1, vc4_ldst16_off(0, 2, 3, 4), "ld r2,(r3+0x4)");
    T(1, vc4_ldst16_off(1, 2, 3, 60), "st r2,(r3+0x3c)");

    /* 32-bit ALU, three registers, with and without a condition */
    for (i = 0; i < (int)(sizeof ops32 / sizeof *ops32); i++) {
        op = ops32[i];
        T(1, vc4_alu32(op, VC4_AL, 20, 21, 22), "%s r20,r21,r22", alu_name[op]);
    }
    /* two-operand ops: mov/neg/not rd,rb (ra = 0); cmp ra,rb (rd = 0) */
    T(1, vc4_alu32(VC4_MOV, VC4_AL, 20, 0, 21), "mov r20,r21");
    T(1, vc4_alu32(VC4_NEG, VC4_AL, 20, 0, 21), "neg r20,r21");
    T(1, vc4_alu32(VC4_NOT, VC4_AL, 20, 0, 21), "not r20,r21");
    T(1, vc4_alu32(VC4_CMP, VC4_AL, 0, 20, 21), "cmp r20,r21");
    T(1, vc4_alu32i(VC4_CMP, VC4_AL, 0, 20, -5), "cmp r20,-5");
    for (cc = 0; cc < 14; cc++) {
        T(1, vc4_alu32i(VC4_MOV, cc, 4, 0, 1), "mov.%s r4,1", cc_name[cc]);
        T(1, vc4_alu32i(VC4_MOV, cc, 17, 0, 0), "mov.%s r17,0", cc_name[cc]);
    }
    T(1, vc4_alu32(VC4_MOV, VC4_AL, 1, 0, VC4_SP), "mov r1,sp");
    T(1, vc4_alu32(VC4_MOV, VC4_AL, VC4_SP, 0, 7), "mov sp,r7");
    T(1, vc4_alu32i(VC4_ADD, VC4_AL, VC4_SP, VC4_SP, -8), "add sp,sp,-8");
    T(1, vc4_alu32i(VC4_ADD, VC4_AL, 2, 7, 31), "add r2,r7,31");
    T(1, vc4_alu32i(VC4_ADD, VC4_AL, 2, 7, -32), "add r2,r7,-32");

    /* 32-bit, 16-bit immediate */
    T(1, vc4_alu32i16(VC4_MOV, 5, 32), "mov r5,32");
    T(1, vc4_alu32i16(VC4_MOV, 20, -32768), "mov r20,-32768");
    T(1, vc4_alu32i16(VC4_MOV, 1, 32767), "mov r1,32767");
    T(1, vc4_alu32i16(VC4_ADD, 1, -1000), "add r1,-1000");

    T(1, vc4_div(0, 0, 0, 1), "div.ss r0,r0,r1");
    T(1, vc4_div(1, 2, 3, 4), "div.uu r2,r3,r4");
    T(1, vc4_div(1, 20, 21, 22), "div.uu r20,r21,r22");

    /* 32-bit loads and stores, signed 12-bit offset. vc4-elf-as picks a
       broken PC-relative form for negative offsets, so objdump only. */
    for (sz = 0; sz < 8; sz++)
        for (i = 0; i < (int)(sizeof offs12 / sizeof *offs12); i++)
            T(offs12[i] > 0, vc4_ldst32(sz, 1, 7, offs12[i]),
              offs12[i] > 0 ? "%s r1,(r7+%d)" : "%s r1,(r7+%d)",
              sz_name[sz], offs12[i]);
    T(1, vc4_ldst32(VC4_ST, 20, VC4_SP, 1024), "st r20,(sp+1024)");

    /* 48 bit */
    T(1, vc4_mov48(5, 0x12345678), "mov r5,0x12345678");
    /* gas picks a shorter form here */
    T(0, vc4_mov48(31 - 8, 0xfffffff8), "mov r23,0xfffffff8");
    T(1, vc4_alu32i16(VC4_AND, 0, 248), "and r0,248");
    T(1, vc4_alu32i16(VC4_CMP, 6, 512), "cmp r6,512");
    T(1, vc4_alu32i16(VC4_OR, 3, -2), "or r3,-2");
    T(0, vc4_alu32i16(VC4_LSR, 3, 7), "lsr r3,7");
    T(1, vc4_alu48(VC4_AND, 2, 0x7fffffff), "and r2,0x7fffffff");
    T(1, vc4_alu48(VC4_CMP, 0, 0x186a0), "cmp r0,0x186a0");
    T(1, vc4_alu48(VC4_OR, 17, 0x80000000), "or r17,0x80000000");
    T(1, vc4_add48(5, 6, 0x12345678), "add r5,r6,0x12345678");
    T(1, vc4_add48(VC4_SP, VC4_SP, 0xffff0000), "add sp,sp,0xffff0000");
    for (i = 0; i < (int)(sizeof offs27 / sizeof *offs27); i++) {
        T(offs27[i] > 0, vc4_ldst48(VC4_LD, 1, 7, offs27[i]), "ld r1,(r7+%d)", offs27[i]);
        T(0, vc4_ldst48(VC4_STB, 9, 25, offs27[i]), "stb r9,(sp+%d)", offs27[i]);
    }

    /* branches: objdump shows the absolute target (binary mode: 0x...) */
    for (cc = 0; cc < 15; cc++) {
        if (cc == VC4_AL)
            continue;
        T(0, vc4_bcc16(cc, -64), "b%s 0x%x", cc_name[cc], start - 64);
        T(0, vc4_bcc32(cc, 0x100000), "b%s 0x%x", cc_name[cc], start + 0x100000);
        T(0, vc4_bcmp_reg(cc, 1, 15, -1024), "b%s r1,r15,0x%x", cc_name[cc], start - 1024);
        T(0, vc4_bcmp_reg(cc, 15, 0, 1022), "b%s r15,r0,0x%x", cc_name[cc], start + 1022);
        T(0, vc4_bcmp_imm(cc, 3, 63, 254), "b%s r3,0x3f,0x%x", cc_name[cc], start + 254);
        T(0, vc4_bcmp_imm(cc, 3, 0, -256), "b%s r3,0x0,0x%x", cc_name[cc], start - 256);
    }
    T(0, vc4_bcc16(VC4_AL, 126), "b 0x%x", start + 126);
    T(0, vc4_bcc32(VC4_AL, -0x800000), "b 0x%x", (start - 0x800000) & 0xffffffff);
    T(0, vc4_bl32(0x3fffffe), "bl 0x%x", start + 0x3fffffe);
    T(0, vc4_bl32(-8), "bl 0x%x", start - 8);

    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;
    fwrite(buf, 1, pos, f);
    fclose(f);
    return 0;
}
