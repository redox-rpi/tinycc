/*
 *  VideoCore IV (VPU) code generator for TinyCC.
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Spike (redox-rpi): integer subset only. ABI as the vc4-toolchain GCC port,
 *  so objects link with GCC's: arguments in r0-r5 then on the stack at the
 *  caller's sp, result in r0 (r0:r1), structs returned through a hidden
 *  pointer in r0, r6-r23 saved by the callee, sp = r25, lr = r26.
 *
 *  Frame:   stm r6-r7,lr,(--sp) ; mov r7,sp ; add sp,sp,-frame
 *           locals at r7 - n, stack arguments at r7 + 12 + 4k
 *  Registers: TCC register n is VC4 register rn. r0-r5 are allocatable,
 *  r6 is the scratch register (saved in the prologue), r7 the frame pointer.
 */
#ifdef TARGET_DEFS_ONLY

#define NB_REGS 7

#define RC_INT     (1 << 0)
#define RC_FLOAT   (1 << 1)        /* no float registers (yet) */
#define RC_R(x)    (1 << (2 + (x))) /* x = 0..5 */

#define RC_IRET    RC_R(0)
#define RC_IRE2    RC_R(1)
#define RC_FRET    RC_R(0)

#define REG_IRET   0
#define REG_IRE2   1
#define REG_FRET   0

#define TREG_R6    6   /* scratch */

/* r7 points VC4_FP_BIAS bytes below the frame's top, so that the first
   locals (r7 + 0..60) fit the 16-bit "ld rd,(rs+off)" form */
#define VC4_FP_BIAS 64

#define PTR_SIZE 4
#define LDOUBLE_SIZE  8
#define LDOUBLE_ALIGN 4
#define MAX_ALIGN     8

#define CHAR_IS_UNSIGNED

/* the callee (GCC) may leave char/short results unextended */
#define PROMOTE_RET

#else /* !TARGET_DEFS_ONLY */

#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char * const target_machine_defs =
    "__vc4__\0"
    "__VC4__\0"
    ;

ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_R(0),
    RC_INT | RC_R(1),
    RC_INT | RC_R(2),
    RC_INT | RC_R(3),
    RC_INT | RC_R(4),
    RC_INT | RC_R(5),
    0, /* r6: scratch */
};

#define FP 7

static int func_sub_sp_offset;

/* store-to-load forwarding: "st rX,(r7+n)" right before "ld rY,(r7+n)"
   with no label in between becomes "mov rY,rX" (or nothing) */
static int st_end = -1, st_reg, st_off;
ST_DATA int vc4_label_ind = -1;   /* last code address that is a jump target */

/* Register variables: locals and parameters whose names tccgen.c's scan
   picked (regvar_ids) live in r8-r15 (callee-saved, saved with r6 and r7) instead of their
   frame slot; regvar_off[i] is the slot (offset) held in r8 + i. Only
   4-byte scalars, and none in variadic functions or functions with stack arguments. */
static int regvar_off[REGVAR_MAX], nb_regvar, regpush_ind, regvar_off_func;

static int vc4_regvar_wanted(int v, CType *type)
{
    int i, bt = type->t & VT_BTYPE;
    if (func_var || regvar_off_func || nb_regvar >= REGVAR_MAX)
        return 0;
    if ((type->t & (VT_VOLATILE | VT_ARRAY | VT_VLA))
        || (bt != VT_INT && bt != VT_PTR))
        return 0;
    v &= ~SYM_FIELD;
    for (i = 0; i < nb_regvar_ids; i++)
        if (regvar_ids[i] == v)
            return 1;
    return 0;
}

ST_FUNC void vc4_local_var(Sym *sym)
{
    if (vc4_regvar_wanted(sym->v, &sym->type))
        regvar_off[nb_regvar++] = sym->c;
}

static int vc4_slot_reg(int fc)
{
    int i;
    for (i = 0; i < nb_regvar; i++)
        if (regvar_off[i] == fc)
            return 8 + i;
    return -1;
}

/* the register of a register variable used as an operand, or -1 */
static int vc4_regvar_of(SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    if (sv->r != (VT_LOCAL | VT_LVAL) || !(bt == VT_INT || bt == VT_PTR))
        return -1;
    return vc4_slot_reg(sv->c.i);
}

static void o16(unsigned h)
{
    int ind1 = ind + 2;
    if (nocode_wanted)
        return;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    write16le(cur_text_section->data + ind, h);
    ind = ind1;
}

ST_FUNC void o(unsigned int c)
{
    o16(c);
}

#define VC4_EMIT16(h) o16(h)
#include "vc4-enc.h"

/* ---- small helpers that pick the shortest form ---- */

static void vc4_mov_reg(int rd, int rs)
{
    if (rd == rs)
        return;
    if (VC4_FAST(rd) && VC4_FAST(rs))
        vc4_alu16(VC4_MOV, rd, rs);
    else
        vc4_alu32(VC4_MOV, VC4_AL, rd, 0, rs);
}

static void vc4_mov_imm(int rd, int32_t v)
{
    if (VC4_FAST(rd) && v >= 0 && v < 32)
        vc4_alu16i(VC4_MOV, rd, v);
    else if (VC4_FITS_S(v, 16))
        vc4_alu32i16(VC4_MOV, rd, v);
    else
        vc4_mov48(rd, v);
}

/* rd = ra + v */
static void vc4_add_imm(int rd, int ra, int32_t v)
{
    if (rd == ra) {
        if (v == 0)
            return;
        if (VC4_FAST(rd) && v > 0 && v < 32) {
            vc4_alu16i(VC4_ADD, rd, v);
            return;
        }
        if (VC4_FAST(rd) && v < 0 && v > -32) {
            vc4_alu16i(VC4_SUB, rd, -v);
            return;
        }
    }
    if (VC4_FITS_S(v, 6))
        vc4_alu32i(VC4_ADD, VC4_AL, rd, ra, v);
    else if (rd == ra && VC4_FITS_S(v, 16))
        vc4_alu32i16(VC4_ADD, rd, v);
    else
        vc4_add48(rd, ra, v);
}

static void vc4_ldst(int sz, int rd, int base, int32_t off)
{
    if (off == 0 && VC4_FAST(rd) && VC4_FAST(base))
        vc4_ldst16(sz, rd, base);
    else if ((sz == VC4_LD || sz == VC4_ST) && VC4_FAST(rd) && VC4_FAST(base)
             && off >= 0 && off <= 60 && !(off & 3))
        vc4_ldst16_off(sz == VC4_ST, rd, base, off);
    else if (VC4_FITS_S(off, 12))
        vc4_ldst32(sz, rd, base, off);
    else if (VC4_FITS_S(off, 27))
        vc4_ldst48(sz, rd, base, off);
    else
        tcc_error("VC4: load/store offset out of range");
}

/* rd = &sym + addend */
static void vc4_sym_addr(int rd, Sym *sym, int addend)
{
    greloca(cur_text_section, sym, ind, R_VC4_IMM32, addend);
    vc4_mov48(rd, 0);
}

static int vc4_cc(int op)
{
    switch (op) {
    case TOK_EQ:  return VC4_EQ;
    case TOK_NE:  return VC4_NE;
    case TOK_ULT: return VC4_LO;
    case TOK_UGE: return VC4_HS;
    case TOK_ULE: return VC4_LS;
    case TOK_UGT: return VC4_HI;
    case TOK_LT:  return VC4_LT;
    case TOK_GE:  return VC4_GE;
    case TOK_LE:  return VC4_LE;
    case TOK_GT:  return VC4_GT;
    }
    tcc_error("VC4: unknown condition %d", op);
    return VC4_AL;
}

/* access size code for a load (sign from the type) or a store */
static int vc4_accsz(CType *type, int is_store)
{
    int align, size = type_size(type, &align);
    int bt = type->t & VT_BTYPE;
    int uns = (type->t & VT_UNSIGNED) || bt == VT_BOOL;

    if (bt == VT_PTR || bt == VT_FUNC)
        size = 4;
    switch (size) {
    case 1: return is_store ? VC4_STB : uns ? VC4_LDB : VC4_LDSB;
    case 2: return is_store ? VC4_STH : uns ? VC4_LDH : VC4_LDSH;
    case 4: return is_store ? VC4_ST : VC4_LD;
    }
    tcc_error("VC4: %d-byte loads and stores not supported yet", size);
    return 0;
}

/* ---- jumps ----
   Forward jumps are 32-bit "b<cc> pcrel23". While unresolved, the offset
   field of each holds the address of the next jump in the chain (0 ends it);
   the condition stays in place. */

static void vc4_put_bcc32(int pos, int cc, int off)
{
    unsigned char *p = cur_text_section->data + pos;
    uint32_t o = (uint32_t)(off >> 1) & 0x7fffff;
    write16le(p, 0x9000 | cc << 8 | (o >> 16));
    write16le(p + 2, o);
}

static int vc4_chain_next(int pos, int *cc)
{
    unsigned char *p = cur_text_section->data + pos;
    unsigned h0 = read16le(p), h1 = read16le(p + 2);
    if (cc)
        *cc = (h0 >> 8) & 15;
    return (int)(((h0 & 0x7f) << 16 | h1) << 1);
}

ST_FUNC void gsym_addr(int t, int a)
{
    int cc, next;
    if (t && a == ind)
        vc4_label_ind = ind;
    while (t) {
        next = vc4_chain_next(t, &cc);
        if (!VC4_FITS_S(a - t, 24))
            tcc_error("VC4: branch out of range");
        vc4_put_bcc32(t, cc, a - t);
        t = next;
    }
}

static int vc4_gjmp_cc(int cc, int t)
{
    int r;
    if (nocode_wanted)
        return t;
    r = ind;
    o16(0);
    o16(0);
    vc4_put_bcc32(r, cc, t);
    return r;
}

ST_FUNC int gjmp(int t)
{
    return vc4_gjmp_cc(VC4_AL, t);
}

ST_FUNC int gjmp_cond(int op, int t)
{
    return vc4_gjmp_cc(vc4_cc(op), t);
}

ST_FUNC void gjmp_addr(int a)
{
    int r = a - ind;
    if (r >= -128 && r < 128)
        vc4_bcc16(VC4_AL, r);
    else
        vc4_bcc32(VC4_AL, r);
}

ST_FUNC int gjmp_append(int n, int t)
{
    int n1, n2, cc;
    if (n) {
        n1 = n;
        while ((n2 = vc4_chain_next(n1, NULL)))
            n1 = n2;
        vc4_chain_next(n1, &cc);
        vc4_put_bcc32(n1, cc, t);
        t = n;
    }
    return t;
}

ST_FUNC void gen_fill_nops(int bytes)
{
    if (bytes & 1)
        tcc_error("VC4: odd code alignment");
    while (bytes > 0) {
        vc4_nop();
        bytes -= 2;
    }
}

/* ---- retargeting into register variables ----
   "op rT,..; mov rV,rT" for a store into a register variable becomes
   "op rV,..": the last instruction that computed a fresh register is
   remembered here, and store() emits it again with rV as target. */

enum { LA_ALU = 1, LA_ALUI, LA_ADD48, LA_MOVI, LA_MOV };
static struct { int pos, end, rd, kind, op, a, b; int32_t c; } la = { -1, -1 };

static void la_set(int pos, int rd, int kind, int op, int a, int b, int32_t c)
{
    if (op == VC4_CMP || op == VC4_BTEST || nocode_wanted)
        return;
    la.pos = pos, la.end = ind, la.rd = rd, la.kind = kind;
    la.op = op, la.a = a, la.b = b, la.c = c;
}

static int la_retarget(int r, int rv)
{
    if (la.end != ind || la.rd != r || vc4_label_ind == ind || nocode_wanted)
        return 0;
    /* nothing follows the instruction: emit it again at its place, its
       length may change */
    ind = la.pos;
    switch (la.kind) {
    case LA_ALU: vc4_alu32(la.op, VC4_AL, rv, la.a, la.b); break;
    case LA_ALUI: vc4_alu32i(la.op, VC4_AL, rv, la.a, la.c); break;
    case LA_ADD48: vc4_add48(rv, la.a, la.c); break;
    case LA_MOVI: vc4_mov_imm(rv, la.c); break;
    case LA_MOV: vc4_mov_reg(rv, la.a); break;
    }
    la.end = -1;
    return 1;
}

/* ---- fused loop branches ----
   "add rd,#i; cmp rd,rs; b<cc> top" at the bottom of a loop becomes one
   "addcmpb<cc> rd,#i,rs,top" (i = 0: "b<cc> rd,rs,top"), as GCC does:
   rd, rs in r0-r15, i in -8..7, top within 10 bits of halfwords. */

static struct { int pos, end, ra, rb; } lc = { -1, -1 };
static struct { int pos, end, rd, d; } li = { -1, -1 };

static void lc_set(int pos, int ra, int rb)
{
    lc.pos = pos, lc.end = ind, lc.ra = ra, lc.rb = rb;
}

ST_FUNC int vc4_loop_branch(int c)
{
    int start, off, imm = 0, rd = lc.ra, rs = lc.rb;

    if (vtop->r != VT_CMP || vtop->jtrue || vtop->jfalse || nocode_wanted
        || vtop->cmp_op <= 1 || lc.end != ind || vc4_label_ind == ind
        || rd > 15 || rs > 15)
        return 0;
    start = lc.pos;
    if (li.end == lc.pos && li.rd == rd && vc4_label_ind != lc.pos)
        start = li.pos, imm = li.d;
    off = (c - start) >> 1;
    if (off < -512 || off > 511)
        return 0;
    ind = start;
    VC4_EMIT16(0x8000 | vc4_cc(vtop->cmp_op) << 8 | (imm & 15) << 4 | rd);
    VC4_EMIT16(0x4000 | rs << 10 | (off & 0x3ff));
    lc.end = li.end = -1;
    vtop--;
    return 1;
}

/* ---- loads and stores ---- */

ST_FUNC void load(int r, SValue *sv)
{
    int fr = sv->r, v = fr & VT_VALMASK, fc = sv->c.i;

    if ((v == VT_LOCAL || v == VT_LLOCAL) && vc4_slot_reg(fc) >= 0) {
        int rr = vc4_slot_reg(fc), sz, sh;
        if (v == VT_LLOCAL || !(fr & VT_LVAL))
            tcc_error("VC4: internal: register variable used as memory");
        /* a cast lvalue such as (uint8_t)v reads the low bytes
           (little-endian): move, then zero or sign extend */
        sz = vc4_accsz(&sv->type, 0);
        {
            int pos = ind;
            vc4_mov_reg(r, rr);
            if (sz == VC4_LD)
                la_set(pos, r, LA_MOV, VC4_MOV, rr, 0, 0);
        }
        sh = (sz == VC4_LDB || sz == VC4_LDSB) ? 24
           : (sz == VC4_LDH || sz == VC4_LDSH) ? 16 : 0;
        if (sh) {
            vc4_alu16i(VC4_SHL, r, sh);
            vc4_alu16i((sz == VC4_LDSB || sz == VC4_LDSH) ? VC4_ASR : VC4_LSR,
                       r, sh);
        }
        return;
    }
    if (fr & VT_LVAL) {
        int base = r, off = 0, sz = vc4_accsz(&sv->type, 0);
        if (v == VT_LOCAL) {
            base = FP, off = fc + VC4_FP_BIAS;
            if (sz == VC4_LD && ind == st_end && vc4_label_ind != ind
                && off == st_off && !(sv->type.t & VT_VOLATILE)) {
                vc4_mov_reg(r, st_reg);
                return;
            }
        } else if (v == VT_LLOCAL) {
            vc4_ldst(VC4_LD, r, FP, fc + VC4_FP_BIAS);
        } else if (v == VT_CONST) {
            if (fr & VT_SYM) {
                /* global: one 48-bit "ld rd,(pc+sym)" as GCC does */
                greloca(cur_text_section, sv->sym, ind, R_VC4_PCREL27, fc);
                if (sz == VC4_LDSB) {
                    vc4_ldst48(VC4_LDB, r, VC4_PC, 0);
                    vc4_alu16i(VC4_SHL, r, 24);
                    vc4_alu16i(VC4_ASR, r, 24);
                } else {
                    vc4_ldst48(sz, r, VC4_PC, 0);
                }
                return;
            }
            vc4_mov_imm(r, fc);
        } else if (v < VT_CONST) {
            base = v;
            /* "add rB,ra,rb; ldb rd,(rB)" -> "ldb rd,(ra+rb)" when rB
               is used nowhere else */
            if ((sz == VC4_LDB || sz == VC4_LDSB) && sv == vtop
                && la.end == ind && la.kind == LA_ALU && la.op == VC4_ADD
                && la.rd == v && vc4_label_ind != ind && !nocode_wanted) {
                SValue *p;
                for (p = vc4_vstack(); p < vtop; p++)
                    if ((p->r & VT_VALMASK) == v || p->r2 == v)
                        break;
                if (p == vtop) {
                    ind = la.pos;
                    la.end = -1;
                    VC4_EMIT16(0xa080 | r);
                    VC4_EMIT16(la.a << 11 | 0x0700 | la.b);
                    if (sz == VC4_LDSB) {
                        vc4_alu16i(VC4_SHL, r, 24);
                        vc4_alu16i(VC4_ASR, r, 24);
                    }
                    return;
                }
            }
        } else {
            tcc_error("VC4: load: unhandled lvalue %x", fr);
        }
        if (sz == VC4_LDSB) {
            /* GCC uses the 32-bit ldsb, but resim does not know it; ldb and
               a shift pair works everywhere and is as long */
            vc4_ldst(VC4_LDB, r, base, off);
            vc4_alu16i(VC4_SHL, r, 24);
            vc4_alu16i(VC4_ASR, r, 24);
        } else {
            vc4_ldst(sz, r, base, off);
        }
    } else if (v == VT_CONST) {
        if (is_float(sv->type.t))
            tcc_error("VC4: floating point not supported yet");
        if (fr & VT_SYM) {
            vc4_sym_addr(r, sv->sym, fc);
        } else {
            int pos = ind;
            vc4_mov_imm(r, fc);
            la_set(pos, r, LA_MOVI, VC4_MOV, 0, 0, fc);
        }
    } else if (v == VT_LOCAL) {
        vc4_add_imm(r, FP, fc + VC4_FP_BIAS);
    } else if (v < VT_CONST) {
        vc4_mov_reg(r, v);
    } else if (v == VT_CMP) {
        int cc = vc4_cc(sv->cmp_op);
        vc4_alu32i(VC4_MOV, cc ^ 1, r, 0, 0);
        vc4_alu32i(VC4_MOV, cc, r, 0, 1);
    } else if ((v & ~1) == VT_JMP) {
        int t = v & 1;
        vc4_mov_imm(r, t);
        vc4_bcc16(VC4_AL, 4);  /* skip the next 16-bit insn */
        gsym(fc);
        vc4_mov_imm(r, t ^ 1);
    } else {
        tcc_error("VC4: load: unhandled value %x", fr);
    }
}

ST_FUNC void store(int r, SValue *sv)
{
    int fr = sv->r & VT_VALMASK, fc = sv->c.i;
    int base, off = 0, sz;

    if ((sv->type.t & VT_BTYPE) == VT_STRUCT)
        tcc_error("VC4: struct store");
    sz = vc4_accsz(&sv->type, 1);
    if ((fr == VT_LOCAL || fr == VT_LLOCAL) && vc4_slot_reg(fc) >= 0) {
        int rv = vc4_slot_reg(fc);
        if (fr == VT_LLOCAL || sz != VC4_ST)
            tcc_error("VC4: internal: register variable used as memory");
        if (sv == vtop - 1 && vtop->r == r && la_retarget(r, rv)) {
            /* the stored value is now the variable itself */
            vtop->r = VT_LOCAL | VT_LVAL;
            vtop->r2 = VT_CONST;
            vtop->c.i = fc;
            vtop->type = sv->type;
            vtop->sym = sv->sym;
            return;
        }
        vc4_mov_reg(rv, r);
        return;
    }
    if (fr == VT_LOCAL) {
        base = FP, off = fc + VC4_FP_BIAS;
    } else if (fr == VT_LLOCAL) {
        vc4_ldst(VC4_LD, TREG_R6, FP, fc + VC4_FP_BIAS);
        base = TREG_R6;
    } else if (fr == VT_CONST) {
        if (sv->r & VT_SYM) {
            greloca(cur_text_section, sv->sym, ind, R_VC4_PCREL27, fc);
            vc4_ldst48(sz, r, VC4_PC, 0);
            return;
        }
        vc4_mov_imm(TREG_R6, fc);
        base = TREG_R6;
    } else if (fr < VT_CONST) {
        base = fr;
    } else {
        tcc_error("VC4: store: unhandled lvalue %x", sv->r);
        return;
    }
    vc4_ldst(sz, r, base, off);
    if (fr == VT_LOCAL && sz == VC4_ST && !nocode_wanted
        && !(sv->type.t & VT_VOLATILE))
        st_end = ind, st_reg = r, st_off = off;
}

/* ---- calls ---- */

static void vc4_check_arg_type(CType *type)
{
    int bt = type->t & VT_BTYPE;
    if (bt == VT_STRUCT || bt == VT_LLONG || is_float(bt))
        tcc_error("VC4: struct, long long and float arguments not supported yet");
}

static void gcall_or_jmp(int is_call)
{
    if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && (vtop->r & VT_SYM)) {
        greloca(cur_text_section, vtop->sym, ind,
                is_call ? R_VC4_PCREL27_MUL2 : R_VC4_PCREL23_MUL2, vtop->c.i);
        if (is_call)
            vc4_bl32(0);
        else
            vc4_bcc32(VC4_AL, 0);
    } else {
        load(TREG_R6, vtop);
        if (is_call)
            vc4_bl_reg(TREG_R6);
        else
            vc4_b_reg(TREG_R6);
    }
}

ST_FUNC void gfunc_call(int nb_args)
{
    int i, nreg = nb_args < 6 ? nb_args : 6;
    int stack = (nb_args - nreg) * 4;

    for (i = 0; i < nb_args; i++)
        vc4_check_arg_type(&vtop[1 + i - nb_args].type);

    if (stack) {
        vc4_add_imm(VC4_SP, VC4_SP, -stack);
        for (i = nreg; i < nb_args; i++) {
            int r;
            vrotb(nb_args - i);
            r = gv(RC_INT);
            vc4_ldst(VC4_ST, r, VC4_SP, (i - nreg) * 4);
            vtop->r = VT_CONST; /* done */
            vtop->c.i = 0;
            vrott(nb_args - i);
        }
    }
    for (i = 0; i < nreg; i++) {
        vrotb(nb_args - i);
        gv(RC_R(i));
        vrott(nb_args - i);
    }
    for (i = 0; i < nreg; i++)
        if (vtop[1 + i - nb_args].r != i)
            tcc_error("VC4: internal: argument %d not in r%d", i, i);

    vrotb(nb_args + 1);
    save_regs(nb_args + 1);
    gcall_or_jmp(1);
    vtop -= nb_args + 1;
    if (stack)
        vc4_add_imm(VC4_SP, VC4_SP, stack);
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
    /* structs always come back through the hidden pointer (as GCC does
       for the sizes checked so far) */
    *ret_align = 1;
    *regsize = 4;
    return 0;
}

/* Variadic functions: the prologue first stores r0-r5 just below the
   caller's stack arguments (as GCC does), so that all arguments lie in one
   row in memory and va_list is a plain pointer (the i386 definition in
   tccdefs.h):
       r7 + 0, 4, 8      saved r6, r7, lr
       r7 + 12 .. 32     r0 .. r5
       r7 + 36 ...       stack arguments                                  */
static int func_regsave;

ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym = func_type->ref;
    int n = 0, addr, i;

    loc = 0;
    func_vc = 0;
    nb_regvar = 0;
    func_regsave = func_var;
    if (func_regsave) {
        /* r0-r5 right below the stack arguments, so va_arg walks both;
           r6, r7, lr saved one by one in a layout of our own (the order
           stm uses is not documented) */
        vc4_add_imm(VC4_SP, VC4_SP, -36);
        for (i = 0; i < 6; i++)
            vc4_ldst(VC4_ST, i, VC4_SP, 12 + i * 4);
        vc4_ldst(VC4_ST, 6, VC4_SP, 0);
        vc4_ldst(VC4_ST, FP, VC4_SP, 4);
        vc4_ldst(VC4_ST, VC4_LR, VC4_SP, 8);
    } else {
        /* widened to r6-r(7+n) by gfunc_epilog for n register variables */
        regpush_ind = ind;
        vc4_push_r6_r7_lr();
    }
    vc4_add_imm(FP, VC4_SP, -VC4_FP_BIAS);
    func_sub_sp_offset = ind;
    vc4_alu32i16(VC4_ADD, VC4_SP, 0); /* patched by gfunc_epilog */
    addr = func_regsave ? 36 : 12;
    /* stack arguments sit above the saved registers, at offsets fixed
       here: a function with any keeps its variables in memory */
    regvar_off_func = 0;
    for (i = (func_vt.t & VT_BTYPE) == VT_STRUCT, sym = func_type->ref;
         (sym = sym->next) != NULL; i++)
        ;
    regvar_off_func = i > 6;
    sym = func_type->ref;

    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        if (func_regsave) {
            func_vc = 12;
        } else {
            loc -= 4;
            vc4_ldst(VC4_ST, 0, FP, loc + VC4_FP_BIAS);
            func_vc = loc;
        }
        n++;
    }
    while ((sym = sym->next) != NULL) {
        vc4_check_arg_type(&sym->type);
        if (n < 6 && func_regsave) {
            gfunc_set_param(sym, 12 + 4 * n++, 0);
        } else if (n < 6) {
            loc -= 4;
            if (vc4_regvar_wanted(sym->v, &sym->type)) {
                regvar_off[nb_regvar] = loc;
                vc4_mov_reg(8 + nb_regvar++, n++);
            } else {
                vc4_ldst(VC4_ST, n++, FP, loc + VC4_FP_BIAS);
            }
            gfunc_set_param(sym, loc, 0);
        } else {
            gfunc_set_param(sym, addr, 0);
            addr += 4;
        }
    }
}

/* ++/-- on a register variable: one "add rv,#d" (and a copy of the old
   value for x++). Returns 0 when tccgen.c's generic path must do it. */
ST_FUNC int vc4_inc_regvar(int post, int d)
{
    int rv = vc4_regvar_of(vtop), r = 0, align;

    if (rv < 0 || nocode_wanted)
        return 0;
    if ((vtop->type.t & VT_BTYPE) == VT_PTR) {
        int s = type_size(pointed_type(&vtop->type), &align);
        if (s <= 0)
            return 0;
        d *= s;
    }
    if (post) {
        r = get_reg(RC_INT);
        vc4_mov_reg(r, rv);
    }
    li.pos = ind;
    vc4_add_imm(rv, rv, d);
    li.end = ind, li.rd = rv, li.d = d;
    if (d < -8 || d > 7)
        li.end = -1;
    if (post) {
        vtop->r = r;
        vtop->r2 = VT_CONST;
    }
    return 1;
}

ST_FUNC void gfunc_epilog(void)
{
    int v = (-loc + 3) & -4, saved_ind;

    vc4_add_imm(VC4_SP, FP, VC4_FP_BIAS);
    if (func_regsave) {
        vc4_ldst(VC4_LD, TREG_R6, VC4_SP, 0);
        vc4_ldst(VC4_LD, VC4_LR, VC4_SP, 8);
        vc4_ldst(VC4_LD, FP, VC4_SP, 4);
        vc4_add_imm(VC4_SP, VC4_SP, 36);
        vc4_rts();
    } else {
        /* r8.. hold the register variables: save them with r6, r7 */
        vc4_pop_r6_r7_pc_n(nb_regvar);
        saved_ind = ind;
        ind = regpush_ind;
        vc4_push_r6_r7_lr_n(nb_regvar);
        ind = saved_ind;
    }
    saved_ind = ind;
    ind = func_sub_sp_offset;
    if (v > 32768)
        tcc_error("VC4: stack frame of %d bytes, more than 32 KB", v);
    vc4_alu32i16(VC4_ADD, VC4_SP, -v);
    ind = saved_ind;
}

ST_FUNC void ggoto(void)
{
    gcall_or_jmp(0);
    vtop--;
}

/* ---- integer operations ---- */

static int vc4_const_op(void)
{
    return (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

ST_FUNC void gen_opi(int op)
{
    int r, fr, c, vop, is_cmp = op >= TOK_ULT && op <= TOK_GT;

    switch (op) {
    case '+': vop = VC4_ADD; break;
    case '-': vop = VC4_SUB; break;
    case '&': vop = VC4_AND; break;
    case '|': vop = VC4_OR; break;
    case '^': vop = VC4_EOR; break;
    case '*': vop = VC4_MUL; break;
    case TOK_SHL: vop = VC4_SHL; break;
    case TOK_SHR: vop = VC4_LSR; break;
    case TOK_SAR: vop = VC4_ASR; break;
    case '/': case TOK_PDIV: case TOK_UDIV:
    case '%': case TOK_UMOD:
        gv2(RC_INT, RC_INT);
        r = vtop[-1].r;
        fr = vtop[0].r;
        vtop--;
        if (op == '%' || op == TOK_UMOD) {
            vc4_div(op == TOK_UMOD, TREG_R6, r, fr);
            vc4_alu16(VC4_MUL, TREG_R6, fr);
            vc4_alu16(VC4_SUB, r, TREG_R6);
        } else {
            vc4_div(op == TOK_UDIV, r, r, fr);
        }
        return;
    default:
        if (!is_cmp)
            tcc_error("VC4: operation %d not supported yet", op);
        vop = VC4_CMP;
        break;
    }

    if (vc4_const_op()) {
        int lr = vc4_regvar_of(vtop - 1);
        c = vtop->c.i;
        if (op == TOK_SHL || op == TOK_SHR || op == TOK_SAR)
            c &= 31;
        if (lr >= 0) {
            /* register variable on the left: three-operand forms */
            if (vop == VC4_SUB)
                vop = VC4_ADD, c = -c;
            if (vop == VC4_CMP) {
                if (VC4_FITS_S(c, 6))
                    vc4_alu32i(VC4_CMP, VC4_AL, 0, lr, c);
                else if (VC4_FITS_S(c, 16))
                    vc4_alu32i16(VC4_CMP, lr, c);
                else
                    vc4_alu48(VC4_CMP, lr, c);
                goto done;
            }
            r = get_reg(RC_INT);
            if (VC4_FITS_S(c, 6)) {
                int pos = ind;
                vc4_alu32i(vop, VC4_AL, r, lr, c);
                la_set(pos, r, LA_ALUI, vop, lr, 0, c);
            } else if (vop == VC4_ADD) {
                int pos = ind;
                vc4_add48(r, lr, c);
                la_set(pos, r, LA_ADD48, vop, lr, 0, c);
            } else {
                vc4_mov_reg(r, lr);
                if (VC4_FITS_S(c, 16))
                    vc4_alu32i16(vop, r, c);
                else
                    vc4_alu48(vop, r, c);
            }
            vtop[-1].r = r;
            vtop[-1].r2 = VT_CONST;
            goto done;
        }
        if (op == '-' && c < 0 && c > -32)
            vop = VC4_ADD, c = -c;
        if (c >= 0 && c < 32 && vc4_op4[vop] >= 0) {
            vswap();
            r = gv(RC_INT);
            vswap();
            {
                int pos = ind;
                vc4_alu16i(vop, r, c);
                la_set(pos, r, LA_ALUI, vop, r, 0, c);
            }
            goto done;
        }
        /* single-bit masks: 16-bit bmask/bset/bclr */
        if (vop == VC4_AND && c != -1 && (c & (c + 1)) == 0) {
            int n = 0;
            while ((uint32_t)c >> n)
                n++;
            vop = VC4_BMASK, c = n;
        } else if (vop == VC4_OR && c && (c & (c - 1)) == 0) {
            int n = 0;
            while ((uint32_t)c >> n != 1)
                n++;
            vop = VC4_BSET, c = n;
        } else if (vop == VC4_AND && ~c && (~c & (~c - 1)) == 0) {
            int n = 0;
            while ((uint32_t)~c >> n != 1)
                n++;
            vop = VC4_BCLR, c = n;
        }
        if (c >= 0 && c < 32 && vc4_op4[vop] >= 0) {
            vswap();
            r = gv(RC_INT);
            vswap();
            {
                int pos = ind;
                vc4_alu16i(vop, r, c);
                la_set(pos, r, LA_ALUI, vop, r, 0, c);
            }
            goto done;
        }
        /* every ALU op has "op rd,#simm16" and "op rd,#imm32" forms */
        if (vop == VC4_SUB)
            vop = VC4_ADD, c = -c;
        vswap();
        r = gv(RC_INT);
        vswap();
        if (vop == VC4_ADD)
            vc4_add_imm(r, r, c);
        else if (vop == VC4_CMP && VC4_FITS_S(c, 6))
            vc4_alu32i(VC4_CMP, VC4_AL, 0, r, c);
        else if (VC4_FITS_S(c, 16))
            vc4_alu32i16(vop, r, c);
        else
            vc4_alu48(vop, r, c);
        goto done;
    }
    {
        int lr = vc4_regvar_of(vtop - 1), rr = vc4_regvar_of(vtop);
        if (lr >= 0 || rr >= 0) {
            fr = rr >= 0 ? rr : gv(RC_INT);
            if (lr >= 0) {
                if (vop == VC4_CMP) {
                    int pos = ind;
                    vc4_alu32(VC4_CMP, VC4_AL, 0, lr, fr);
                    lc_set(pos, lr, fr);
                    goto done;
                }
                int pos;
                r = get_reg(RC_INT);
                pos = ind;
                vc4_alu32(vop, VC4_AL, r, lr, fr);
                la_set(pos, r, LA_ALU, vop, lr, fr, 0);
                vtop[-1].r = r;
                vtop[-1].r2 = VT_CONST;
            } else {
                vswap();
                r = gv(RC_INT);
                vswap();
                int pos = ind;
                vc4_alu32(vop, VC4_AL, vop == VC4_CMP ? 0 : r, r, fr);
                la_set(pos, r, LA_ALU, vop, r, fr, 0);
                if (vop == VC4_CMP)
                    lc_set(pos, r, fr);
            }
            goto done;
        }
    }
    gv2(RC_INT, RC_INT);
    r = vtop[-1].r;
    fr = vtop[0].r;
    {
        int pos = ind;
        vc4_alu16(vop, r, fr);
        la_set(pos, r, LA_ALU, vop, r, fr, 0);
        if (vop == VC4_CMP)
            lc_set(pos, r, fr);
    }
done:
    vtop--;
    if (is_cmp)
        vset_VT_CMP(op);
}

ST_FUNC void gen_opf(int op)
{
    tcc_error("VC4: floating point not supported yet");
}

ST_FUNC void gen_cvt_itof(int t)
{
    tcc_error("VC4: floating point not supported yet");
}

ST_FUNC void gen_cvt_ftoi(int t)
{
    tcc_error("VC4: floating point not supported yet");
}

ST_FUNC void gen_cvt_ftof(int t)
{
    tcc_error("VC4: floating point not supported yet");
}

ST_FUNC void gen_vla_sp_save(int addr)
{
    tcc_error("VC4: variable-length arrays not supported yet");
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    tcc_error("VC4: variable-length arrays not supported yet");
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    tcc_error("VC4: variable-length arrays not supported yet");
}

#endif /* !TARGET_DEFS_ONLY */
