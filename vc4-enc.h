/*
 *  VideoCore IV (VPU) instruction encoder for TinyCC.
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Our own encoder. The bit layouts were worked out from what vc4-elf-as
 *  writes and vc4-elf-objdump reads (used as black boxes), and every form
 *  is checked against both by tests/vc4/enc-test.py. No tables or code are
 *  taken from GNU binutils. Instructions are sequences of little-endian
 *  16-bit halfwords; the first halfword selects the length (16, 32, 48 bit).
 *
 *  The user of this header defines VC4_EMIT16(h), which appends one
 *  halfword to the output.
 */
#ifndef VC4_ENC_H
#define VC4_ENC_H

#include <stdint.h>

/* registers */
enum {
    VC4_R0 = 0, VC4_R6 = 6, VC4_R7 = 7,
    VC4_GP = 24, VC4_SP = 25, VC4_LR = 26, VC4_SR = 30, VC4_PC = 31
};

/* condition codes (h-cond) */
enum {
    VC4_EQ, VC4_NE, VC4_LO, VC4_HS, VC4_MI, VC4_PL, VC4_VS, VC4_VC,
    VC4_HI, VC4_LS, VC4_GE, VC4_LT, VC4_GT, VC4_LE, VC4_AL, VC4_NV
};

/* the 5-bit ALU opcodes (16-bit register form, all 32-bit forms) */
enum {
    VC4_MOV, VC4_CMN, VC4_ADD, VC4_BIC, VC4_MUL, VC4_EOR, VC4_SUB, VC4_AND,
    VC4_NOT, VC4_ROR, VC4_CMP, VC4_RSUB, VC4_BTEST, VC4_OR, VC4_BMASK, VC4_MAX,
    VC4_BSET, VC4_MIN, VC4_BCLR, VC4_ADDS2, VC4_BCHG, VC4_ADDS4, VC4_ADDS8,
    VC4_ADDS16, VC4_SIGNEXT, VC4_NEG, VC4_LSR, VC4_MSB, VC4_SHL, VC4_BREV,
    VC4_ASR, VC4_ABS
};

/* the 4-bit opcodes of the 16-bit "op rd, #uimm5" form */
static const signed char vc4_op4[32] = {
    /* MOV */ 0, /* CMN */ -1, /* ADD */ 1, /* BIC */ -1, /* MUL */ 2,
    /* EOR */ -1, /* SUB */ 3, /* AND */ -1, /* NOT */ 4, /* ROR */ -1,
    /* CMP */ 5, /* RSUB */ -1, /* BTEST */ 6, /* OR */ -1, /* BMASK */ 7,
    /* MAX */ -1, /* BSET */ 8, /* MIN */ -1, /* BCLR */ 9, /* ADDS2 */ -1,
    /* BCHG */ 10, /* ADDS4 */ -1, /* ADDS8 */ 11, /* ADDS16 */ -1,
    /* SIGNEXT */ 12, /* NEG */ -1, /* LSR */ 13, /* MSB */ -1, /* SHL */ 14,
    /* BREV */ -1, /* ASR */ 15, /* ABS */ -1
};

/* load/store access size (accsz) */
enum {
    VC4_LD, VC4_ST, VC4_LDH, VC4_STH, VC4_LDB, VC4_STB, VC4_LDSH, VC4_LDSB
};

#define VC4_FAST(r)     ((unsigned)(r) < 16)
#define VC4_FITS_S(v,n) ((int32_t)(v) >= -(1 << ((n) - 1)) && (int32_t)(v) < (1 << ((n) - 1)))
#define VC4_FITS_U(v,n) ((uint32_t)(v) < (1u << (n)))

/* ---- 16 bit ---- */

static inline void vc4_nop(void) { VC4_EMIT16(0x0001); }
static inline void vc4_rts(void) { VC4_EMIT16(0x005a); }
/* b rN / bl rN */
static inline void vc4_b_reg(int r) { VC4_EMIT16(0x0040 | r); }
static inline void vc4_bl_reg(int r) { VC4_EMIT16(0x0060 | r); }

/* stm r6-r7,lr,(--sp) and ldm r6-r7,pc,(sp++) */
static inline void vc4_push_r6_r7_lr(void) { VC4_EMIT16(0x03a1); }
static inline void vc4_pop_r6_r7_pc(void) { VC4_EMIT16(0x0321); }
/* stm r6-r(7+n),lr,(--sp) and ldm r6-r(7+n),pc,(sp++), n = 0..24 */
static inline void vc4_push_r6_r7_lr_n(int n) { VC4_EMIT16(0x03a1 + n); }
static inline void vc4_pop_r6_r7_pc_n(int n) { VC4_EMIT16(0x0321 + n); }

/* op rd, rs  (rd, rs in r0..r15) */
static inline void vc4_alu16(int op, int rd, int rs)
{
    VC4_EMIT16(0x4000 | op << 8 | rs << 4 | rd);
}

/* op rd, #imm (rd in r0..r15, imm 0..31, op with a 4-bit code) */
static inline void vc4_alu16i(int op, int rd, int imm)
{
    VC4_EMIT16(0x6000 | vc4_op4[op] << 9 | imm << 4 | rd);
}

/* ld/st rd, (ra)  (rd, ra in r0..r15) */
static inline void vc4_ldst16(int sz, int rd, int ra)
{
    VC4_EMIT16(0x0800 | sz << 8 | ra << 4 | rd);
}

/* ld/st rd, (ra + off), word only, off = 0..60 multiple of 4 */
static inline void vc4_ldst16_off(int st, int rd, int ra, int off)
{
    VC4_EMIT16(0x2000 | st << 12 | (off >> 2) << 8 | ra << 4 | rd);
}

/* b<cc> with a 7-bit halfword offset: off in bytes from this insn */
static inline void vc4_bcc16(int cc, int off)
{
    VC4_EMIT16(0x1800 | cc << 7 | ((off >> 1) & 0x7f));
}

/* ---- 32 bit ---- */

/* op<cc> rd, ra, rb */
static inline void vc4_alu32(int op, int cc, int rd, int ra, int rb)
{
    VC4_EMIT16(0xc000 | op << 5 | rd);
    VC4_EMIT16(ra << 11 | cc << 7 | rb);
}

/* op<cc> rd, ra, #simm6 */
static inline void vc4_alu32i(int op, int cc, int rd, int ra, int imm)
{
    VC4_EMIT16(0xc000 | op << 5 | rd);
    VC4_EMIT16(ra << 11 | cc << 7 | 0x40 | (imm & 0x3f));
}

/* op rd, #simm16 (two-operand: rd = rd op imm; mov: rd = imm) */
static inline void vc4_alu32i16(int op, int rd, int imm)
{
    VC4_EMIT16(0xb000 | op << 5 | rd);
    VC4_EMIT16(imm & 0xffff);
}

/* div.ss / div.uu rd, ra, rb */
static inline void vc4_div(int is_unsigned, int rd, int ra, int rb)
{
    VC4_EMIT16((is_unsigned ? 0xc4e0 : 0xc480) | rd);
    VC4_EMIT16(ra << 11 | VC4_AL << 7 | rb);
}

/* ld/st rd, (ra + simm12) */
static inline void vc4_ldst32(int sz, int rd, int ra, int off)
{
    VC4_EMIT16(0xa200 | ((off >> 11) & 1) << 8 | sz << 5 | rd);
    VC4_EMIT16(ra << 11 | (off & 0x7ff));
}

/* b<cc> ra, rb, target  (ra, rb in r0..r15; off in bytes, +-1 KiB) */
static inline void vc4_bcmp_reg(int cc, int ra, int rb, int off)
{
    VC4_EMIT16(0x8000 | cc << 8 | ra);
    VC4_EMIT16(0x4000 | rb << 10 | ((off >> 1) & 0x3ff));
}

/* b<cc> ra, #uimm6, target  (off in bytes, +-256 bytes) */
static inline void vc4_bcmp_imm(int cc, int ra, int imm, int off)
{
    VC4_EMIT16(0x8000 | cc << 8 | ra);
    VC4_EMIT16(0xc000 | imm << 8 | ((off >> 1) & 0xff));
}

/* b<cc> target with a 23-bit halfword offset (off in bytes, +-8 MiB) */
static inline void vc4_bcc32(int cc, int off)
{
    uint32_t o = (uint32_t)(off >> 1) & 0x7fffff;
    VC4_EMIT16(0x9000 | cc << 8 | (o >> 16));
    VC4_EMIT16(o & 0xffff);
}

/* bl target with a 27-bit halfword offset (off in bytes) */
static inline void vc4_bl32(int off)
{
    uint32_t o = (uint32_t)(off >> 1) & 0x7ffffff;
    VC4_EMIT16(0x9080 | (o >> 23) << 8 | ((o >> 16) & 0x7f));
    VC4_EMIT16(o & 0xffff);
}

/* ---- 48 bit ---- */

/* mov rd, #imm32 */
static inline void vc4_mov48(int rd, uint32_t imm)
{
    VC4_EMIT16(0xe800 | rd);
    VC4_EMIT16(imm & 0xffff);
    VC4_EMIT16(imm >> 16);
}

/* op rd, #imm32 (two-operand, as the 32-bit #simm16 form) */
static inline void vc4_alu48(int op, int rd, uint32_t imm)
{
    VC4_EMIT16(0xe800 | op << 5 | rd);
    VC4_EMIT16(imm & 0xffff);
    VC4_EMIT16(imm >> 16);
}

/* add rd, ra, #imm32 */
static inline void vc4_add48(int rd, int ra, uint32_t imm)
{
    VC4_EMIT16(0xec00 | ra << 5 | rd);
    VC4_EMIT16(imm & 0xffff);
    VC4_EMIT16(imm >> 16);
}

/* ld/st rd, (ra + simm27) */
static inline void vc4_ldst48(int sz, int rd, int ra, int off)
{
    uint32_t w = ((uint32_t)off & 0x7ffffff) | (uint32_t)ra << 27;
    VC4_EMIT16(0xe600 | sz << 5 | rd);
    VC4_EMIT16(w & 0xffff);
    VC4_EMIT16(w >> 16);
}

#endif /* VC4_ENC_H */
