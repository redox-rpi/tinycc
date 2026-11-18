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

/* r0-r5 are allocated; r6-r15 have no class: r8-r15 can stand on the
   value stack as the old value of a pointer register variable after p++
   (see vc4_inc_regvar) */
#define NB_REGS 16

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

ST_DATA int reg_classes[NB_REGS] = {
    RC_INT | RC_R(0),
    RC_INT | RC_R(1),
    RC_INT | RC_R(2),
    RC_INT | RC_R(3),
    RC_INT | RC_R(4),
    RC_INT | RC_R(5),
    0, /* r6: scratch */
    0, 0, 0, 0, 0, 0, 0, 0, 0 /* r7 (frame), r8-r15 (register variables) */
};

#define FP 7

static int func_sub_sp_offset;

/* store-to-load forwarding: "st rX,(r7+n)" right before "ld rY,(r7+n)"
   with no label in between becomes "mov rY,rX" (or nothing) */
static int st_end = -1, st_reg, st_off;
ST_DATA int vc4_label_ind = -1;   /* last code address that is a jump target */

/* the last compare: "cmp ra,rb" (rb >= 0) or "cmp ra,#imm" (rb < 0), for
   the fused compare-and-branch forms */
static struct { int pos, end, ra, rb, imm; } lc = { -1, -1 };

/* branch sites of the current function, for vc4_relax(); a B32 site
   with cmp >= 0 directly follows that compare */
/* BR_DEL is no branch: imm bytes at pos that are dropped (the frame
   set-up of a function that does not use its frame) */
enum { BR_B32 = 1, BR_B16, BR_CREG, BR_CIMM, BR_DEL };
typedef struct { int pos, kind, cmp, ra, rb, imm, cc, tgt, st; } VC4Br;
static VC4Br *brs;
static int nb_brs, max_brs, fn_start, fn_reloc, fn_asm;
static int fp_used;   /* r7 (the frame) read since the prologue */
static int r6_used;   /* r6 (scratch, callee-saved) written */

/* Leaf functions (no call, see regvar_leaf): the first VC4_LEAF_ARGS
   parameters that the scan picked stay in their argument register, which
   is then not allocated (r3-r5 remain); with no other register variable
   and no frame there is nothing to save, and the function is its body
   and "rts", as GCC's. */
#define VC4_LEAF_ARGS 3
static int fn_leaf, argrv_off[VC4_LEAF_ARGS], argrv_reg[VC4_LEAF_ARGS], nb_argrv;

/* a pending p++ on the pointer register variable rv (vc4_inc_regvar) */
static struct { int rv, d, on; } pinc;
static void vc4_inc_flush_sv(SValue *extra);

static void vc4_br_add(int pos, int kind, int fuse)
{
    VC4Br *b;
    if (nocode_wanted)
        return;
    if (nb_brs == max_brs) {
        max_brs = max_brs ? 2 * max_brs : 64;
        brs = tcc_realloc(brs, max_brs * sizeof *brs);
    }
    b = &brs[nb_brs++];
    b->pos = pos, b->kind = kind, b->cmp = -1, b->st = 0;
    if (fuse && lc.end == pos && vc4_label_ind != pos)
        b->cmp = lc.pos, b->ra = lc.ra, b->rb = lc.rb, b->imm = lc.imm;
}

/* Register variables: locals and parameters whose names tccgen.c's scan
   picked (regvar_ids) live in r8-r15 (callee-saved, saved with r6 and r7) instead of their
   frame slot; regvar_off[i] is the slot (offset) held in r8 + i. Only
   4-byte scalars, and none in variadic functions or functions with stack arguments. */
static int regvar_off[REGVAR_MAX], nb_regvar, regpush_ind, regvar_off_func;

static int vc4_regvar_name(int v, CType *type)
{
    int i, bt = type->t & VT_BTYPE;
    if (func_var || regvar_off_func)
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

static int vc4_regvar_wanted(int v, CType *type)
{
    return nb_regvar < REGVAR_MAX && vc4_regvar_name(v, type);
}

ST_FUNC void vc4_local_var(Sym *sym)
{
    if (vc4_regvar_wanted(sym->v, &sym->type))
        regvar_off[nb_regvar++] = sym->c;
}

static int vc4_slot_reg(int fc)
{
    int i;
    for (i = 0; i < nb_argrv; i++)
        if (argrv_off[i] == fc)
            return argrv_reg[i];
    for (i = 0; i < nb_regvar; i++)
        if (regvar_off[i] == fc)
            return 8 + i;
    return -1;
}

/* the register of a register variable used as an operand, or -1 */
static int vc4_regvar_of(SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    int r;
    if (sv->r != (VT_LOCAL | VT_LVAL) || !(bt == VT_INT || bt == VT_PTR))
        return -1;
    r = vc4_slot_reg(sv->c.i);
    if (r >= 0 && pinc.on && r == pinc.rv)
        vc4_inc_flush_sv(NULL);   /* p is read: p++ first */
    return r;
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
    if (ra == FP)
        fp_used = 1;
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
    if (base == FP)
        fp_used = 1;
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
    if (t && a == ind) {
        if (pinc.on) {
            vc4_inc_flush_sv(NULL);   /* on the path falling into the label */
            a = ind;
        }
        vc4_label_ind = ind;
    }
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
    vc4_inc_flush_sv(NULL);
    r = ind;
    o16(0);
    o16(0);
    vc4_put_bcc32(r, cc, t);
    vc4_br_add(r, BR_B32, cc != VC4_AL);
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
    int r, pos;
    vc4_inc_flush_sv(NULL);
    pos = ind, r = a - ind;
    if (r >= -128 && r < 128) {
        vc4_bcc16(VC4_AL, r);
        vc4_br_add(pos, BR_B16, 0);
    } else {
        vc4_bcc32(VC4_AL, r);
        vc4_br_add(pos, BR_B32, 0);
    }
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
   rd, rs in r0-r15, i in -8..7, top within 10 bits of halfwords. A
   compare with a constant 0..63 gives "addcmpb<cc> rd,#i,#imm,top", top
   within 8 bits of halfwords. */

static struct { int pos, end, rd, d; } li = { -1, -1 };

static void lc_set(int pos, int ra, int rb)
{
    lc.pos = pos, lc.end = ind, lc.ra = ra, lc.rb = rb, lc.imm = 0;
}

static void lc_set_imm(int pos, int ra, int imm)
{
    lc.pos = pos, lc.end = ind, lc.ra = ra, lc.rb = -1, lc.imm = imm;
}

ST_FUNC int vc4_loop_branch(int c)
{
    int start, off, imm = 0, rd, rs;

    vc4_inc_flush_sv(NULL);
    rd = lc.ra, rs = lc.rb;
    if (vtop->r != VT_CMP || vtop->jtrue || vtop->jfalse || nocode_wanted
        || vtop->cmp_op <= 1 || lc.end != ind || vc4_label_ind == ind
        || rd > 15 || rs > 15 || (rs < 0 && (lc.imm < 0 || lc.imm > 63)))
        return 0;
    start = lc.pos;
    if (li.end == lc.pos && li.rd == rd && vc4_label_ind != lc.pos)
        start = li.pos, imm = li.d;
    off = (c - start) >> 1;
    if (rs >= 0 ? off < -512 || off > 511 : off < -128 || off > 127)
        return 0;
    ind = start;
    if (rs >= 0)
        vc4_addcmpb_reg(vc4_cc(vtop->cmp_op), rd, imm, rs, off * 2);
    else
        vc4_addcmpb_imm(vc4_cc(vtop->cmp_op), rd, imm, lc.imm, off * 2);
    vc4_br_add(start, rs >= 0 ? BR_CREG : BR_CIMM, 0);
    lc.end = li.end = -1;
    vtop--;
    return 1;
}

/* ---- postponed p++ on pointer register variables ----
   "*p++ = v" and "v = *p++" with p in a register become one
   "st/ld v,(rp++)" (as GCC's auto-inc-dec). p++ leaves the old value on
   the value stack as the register rp itself and the increment pending;
   a load or store through it with the pointer's access size folds the
   increment in. Anything else that could see p (a read or write of p, a
   jump, a label, a call, the end of the statement or a dropped value)
   first emits the increment, after copying the old value where the
   value stack still holds it. */

static int vc4_inc_refs(SValue *skip)
{
    SValue *p;
    int n = 0;
    for (p = vc4_vstack(); p <= vtop; p++)
        if (p != skip && ((p->r & VT_VALMASK) == pinc.rv || p->r2 == pinc.rv))
            n++;
    return n;
}

static int vc4_on_vstack(SValue *sv)
{
    return sv >= vc4_vstack() && sv <= vtop;
}

static void vc4_inc_fix(SValue *p, int *r)
{
    if ((p->r & VT_VALMASK) != pinc.rv && p->r2 != pinc.rv)
        return;
    if (*r < 0) {
        *r = get_reg(RC_INT);
        vc4_mov_reg(*r, pinc.rv);
    }
    if ((p->r & VT_VALMASK) == pinc.rv)
        p->r = (p->r & ~VT_VALMASK) | *r;
    if (p->r2 == pinc.rv)
        p->r2 = *r;
}

static void vc4_inc_flush_sv(SValue *extra)
{
    SValue *p;
    int r = -1, cmp_len = 0;
    unsigned char cmp[8];

    if (!pinc.on || nocode_wanted)
        return;
    pinc.on = 0;
    /* a compare right before: keep it next to its branch, in case the
       increment touches the flags */
    if (lc.end == ind && vc4_label_ind != ind && ind - lc.pos <= 8) {
        cmp_len = ind - lc.pos;
        memcpy(cmp, cur_text_section->data + lc.pos, cmp_len);
        ind = lc.pos;
    }
    for (p = vc4_vstack(); p <= vtop; p++)
        vc4_inc_fix(p, &r);
    if (extra && !vc4_on_vstack(extra))
        vc4_inc_fix(extra, &r);
    li.pos = ind;
    vc4_add_imm(pinc.rv, pinc.rv, pinc.d);
    li.end = ind, li.rd = pinc.rv, li.d = pinc.d;
    if (cmp_len) {
        int i;
        lc.pos = ind;
        for (i = 0; i < cmp_len; i += 2)
            o16(read16le(cmp + i));
        lc.end = ind;
        li.end = -1;
    }
    la.end = -1;
}

ST_FUNC void vc4_inc_flush(void)
{
    vc4_inc_flush_sv(NULL);
}

/* vpop: a dropped p++ (as in "p++;") still increments p */
ST_FUNC void vc4_vpop(void)
{
    if (pinc.on && !nocode_wanted
        && ((vtop->r & VT_VALMASK) == pinc.rv || vtop->r2 == pinc.rv)) {
        vtop->r = vtop->r2 = VT_CONST;
        vc4_inc_flush_sv(NULL);
    }
}

ST_FUNC void vc4_note_asm(void)
{
    vc4_inc_flush_sv(NULL);
    fn_asm = 1;
    fp_used = r6_used = 1;   /* the asm may use r6, r7 */
}

/* &&label: the label's address goes into data or code through a symbol
   that tccgen.c defines only after gfunc_epilog(), so vc4_relax() cannot
   see it; the function keeps its code as emitted */
ST_FUNC void vc4_note_label_addr(void)
{
    fn_asm = 1;
}

/* access size in bytes of a load/store size code */
static int vc4_sz_bytes(int sz)
{
    return sz == VC4_LD || sz == VC4_ST ? 4
         : sz == VC4_LDH || sz == VC4_STH || sz == VC4_LDSH ? 2 : 1;
}

/* ---- loads and stores ---- */

ST_FUNC void load(int r, SValue *sv)
{
    int fr = sv->r, v = fr & VT_VALMASK, fc = sv->c.i;

    if (pinc.on && !nocode_wanted) {
        if (v == pinc.rv && (fr & VT_LVAL) && vc4_on_vstack(sv)
            && !vc4_inc_refs(sv)) {
            int sz = vc4_accsz(&sv->type, 0);
            if (vc4_sz_bytes(sz) == pinc.d) {
                /* v = *p++ */
                pinc.on = 0;
                vc4_ldst_postinc(sz == VC4_LDSB ? VC4_LDB
                                 : sz == VC4_LDSH ? VC4_LDH : sz, r, pinc.rv);
                if (sz == VC4_LDSB || sz == VC4_LDSH)
                    vc4_alu16i(VC4_SIGNEXT, r, sz == VC4_LDSB ? 7 : 15);
                return;
            }
        }
        if (((fr & VT_LVAL) && v == pinc.rv) || (v & ~1) == VT_JMP
            || (v == VT_LOCAL && vc4_slot_reg(fc) == pinc.rv)) {
            vc4_inc_flush_sv(sv);
            fr = sv->r, v = fr & VT_VALMASK;
        }
    }

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
        sh = (sz == VC4_LDB || sz == VC4_LDSB) ? 8
           : (sz == VC4_LDH || sz == VC4_LDSH) ? 16 : 0;
        if (sh) {
            if (sz == VC4_LDSB || sz == VC4_LDSH)
                vc4_alu16i(VC4_SIGNEXT, r, sh - 1);
            else
                vc4_alu16i(VC4_BMASK, r, sh);
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
                    vc4_alu16i(VC4_SIGNEXT, r, 7);
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
                    if (sz == VC4_LDSB)
                        vc4_alu16i(VC4_SIGNEXT, r, 7);
                    return;
                }
            }
        } else {
            tcc_error("VC4: load: unhandled lvalue %x", fr);
        }
        if (sz == VC4_LDSB) {
            /* GCC uses the 32-bit ldsb, but resim does not know it: ldb
               and a 16-bit signext */
            vc4_ldst(VC4_LDB, r, base, off);
            vc4_alu16i(VC4_SIGNEXT, r, 7);
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
        int t = v & 1, pos;
        vc4_mov_imm(r, t);
        pos = ind;
        vc4_bcc16(VC4_AL, 4);  /* skip the next 16-bit insn */
        vc4_br_add(pos, BR_B16, 0);
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
    if (pinc.on && !nocode_wanted) {
        if (fr == pinc.rv && (sv->r & VT_LVAL) && r != pinc.rv
            && vc4_sz_bytes(sz) == pinc.d && vc4_on_vstack(sv)
            && !vc4_inc_refs(sv)) {
            /* *p++ = v */
            pinc.on = 0;
            vc4_ldst_postinc(sz, r, pinc.rv);
            return;
        }
        if (fr == pinc.rv || ((fr == VT_LOCAL || fr == VT_LLOCAL)
                              && vc4_slot_reg(fc) == pinc.rv)) {
            vc4_inc_flush_sv(sv);
            fr = sv->r & VT_VALMASK;
        }
    }
    if ((fr == VT_LOCAL || fr == VT_LLOCAL) && vc4_slot_reg(fc) >= 0) {
        int rv = vc4_slot_reg(fc);
        if (fr == VT_LLOCAL || sz != VC4_ST) {
            /* a compound literal in typeof or sizeof gets size 0 and may
               sit on a register variable's slot; no code is made */
            if (nocode_wanted)
                return;
            tcc_error("VC4: internal: register variable used as memory");
        }
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
        r6_used = 1;
    } else if (fr == VT_CONST) {
        if (sv->r & VT_SYM) {
            greloca(cur_text_section, sv->sym, ind, R_VC4_PCREL27, fc);
            vc4_ldst48(sz, r, VC4_PC, 0);
            return;
        }
        vc4_mov_imm(TREG_R6, fc);
        base = TREG_R6;
        r6_used = 1;
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

    if (fn_leaf)
        tcc_error("VC4: internal: a call in %s, taken as a leaf function", funcname);
    vc4_inc_flush_sv(NULL);
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
    for (i = 0; i < 6; i++)   /* after an error in the last function */
        reg_classes[i] |= RC_INT;
    fn_start = ind;
    nb_brs = 0;
    fn_asm = 0;
    pinc.on = 0;
    fn_reloc = cur_text_section->reloc ? cur_text_section->reloc->data_offset : 0;
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
    fp_used = 0;
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
    i = func_vt.t & VT_BTYPE;
    fn_leaf = regvar_leaf && !func_regsave && !regvar_off_func
        && i != VT_STRUCT && i != VT_LLONG && !is_float(i)
        && !tcc_state->do_bounds_check && !tcc_state->test_coverage;
    nb_argrv = 0;
    r6_used = 0;

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
            if (fn_leaf && n < VC4_LEAF_ARGS
                && vc4_regvar_name(sym->v, &sym->type)) {
                /* stays in rn, which is no longer allocated */
                argrv_off[nb_argrv] = loc;
                argrv_reg[nb_argrv++] = n;
                reg_classes[n] &= ~RC_INT;
                n++;
            } else if (vc4_regvar_wanted(sym->v, &sym->type)) {
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
    vc4_inc_flush_sv(NULL);   /* an earlier p++ */
    if ((vtop->type.t & VT_BTYPE) == VT_PTR) {
        int s = type_size(pointed_type(&vtop->type), &align);
        if (s <= 0)
            return 0;
        d *= s;
        if (post && d == s && (s == 1 || s == 2 || s == 4)) {
            /* p++: the old value is rv itself, the increment waits */
            pinc.rv = rv, pinc.d = d, pinc.on = 1;
            vtop->r = rv;
            vtop->r2 = VT_CONST;
            return 1;
        }
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

/* ---- branch relaxation at the end of a function ----
   Forward jumps are emitted as 32-bit "b<cc>" because the target is not
   known yet. Once the function is complete: "cmp ra,rb|#imm; b<cc> L"
   becomes one fused "b<cc> ra,rb|#imm,L" (as GCC emits it, ra/rb in
   r0-r15, imm 0..63, L within 1 KB, or 256 bytes with imm), and other
   jumps within 128 bytes become 16-bit. Every edit only shrinks code, so
   distances only shrink; sites that do not fit after a pass go back to
   the longer form until nothing changes. Not done for functions with
   inline asm, debug or coverage code, or symbols defined inside them. */

static int vc4_br_cmp(const void *a, const void *b)
{
    const VC4Br *x = a, *y = b;
    return x->pos - y->pos;
}

static int vc4_sext(unsigned v, int bits)
{
    return (int)(v << (32 - bits)) >> (32 - bits);
}

/* the edited region of a site in state st: [start, end), new length */
static int vc4_br_start(VC4Br *b) { return b->st == 2 ? b->cmp : b->pos; }
static int vc4_br_end(VC4Br *b)
{
    return b->pos + (b->kind == BR_DEL ? b->imm : b->kind == BR_B16 ? 2 : 4);
}
static int vc4_br_len(VC4Br *b)
{
    return b->kind == BR_DEL ? 0
         : b->kind == BR_B16 || (b->kind == BR_B32 && b->st == 1) ? 2 : 4;
}

/* new address of the old address x; sav[i] = bytes saved by sites < i */
static int vc4_relax_map(int x, int *sav)
{
    int lo = 0, hi = nb_brs;   /* first site with start >= x */
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (vc4_br_start(&brs[m]) < x)
            lo = m + 1;
        else
            hi = m;
    }
    return x - sav[lo];
}

static void vc4_relax(void)
{
    unsigned char *d = cur_text_section->data, *old;
    int i, j, n, changed, end = ind, *sav;
    ElfW(Sym) *sym;
    ElfW_Rel *rel;

    if (nocode_wanted || fn_asm || tcc_state->do_debug
        || tcc_state->test_coverage || nb_brs == 0)
        goto reset;
    /* symbols (labels whose address is taken) inside the function */
    n = symtab_section->data_offset / sizeof(ElfW(Sym));
    sym = (ElfW(Sym) *)symtab_section->data;
    for (i = 1; i < n; i++)
        if (sym[i].st_shndx == cur_text_section->sh_num
            && sym[i].st_value > (unsigned)fn_start && sym[i].st_value < (unsigned)end)
            goto reset;

    qsort(brs, nb_brs, sizeof *brs, vc4_br_cmp);
    for (i = j = 0; i < nb_brs; i++)   /* drop duplicates, stale sites */
        if (brs[i].pos >= fn_start && brs[i].pos < end
            && (j == 0 || brs[j - 1].pos != brs[i].pos))
            brs[j++] = brs[i];
    nb_brs = j;

    /* decode */
    for (i = 0; i < nb_brs; i++) {
        VC4Br *b = &brs[i];
        unsigned h0 = read16le(d + b->pos), h1;
        if (vc4_br_end(b) > end)
            goto reset;
        if (b->kind == BR_DEL) {
            b->tgt = -1;
            continue;
        }
        h1 = b->kind == BR_B16 ? 0 : read16le(d + b->pos + 2);
        switch (b->kind) {
        case BR_B32:
            if ((h0 & 0xf080) != 0x9000)
                goto reset;
            b->cc = (h0 >> 8) & 15;
            b->tgt = b->pos + 2 * vc4_sext((h0 & 0x7f) << 16 | h1, 23);
            break;
        case BR_B16:
            if ((h0 & 0xf800) != 0x1800)
                goto reset;
            b->cc = (h0 >> 7) & 15;
            b->tgt = b->pos + 2 * vc4_sext(h0 & 0x7f, 7);
            break;
        case BR_CREG:
            if ((h0 & 0xf000) != 0x8000 || (h1 & 0xc000) != 0x4000)
                goto reset;
            b->tgt = b->pos + 2 * vc4_sext(h1 & 0x3ff, 10);
            break;
        case BR_CIMM:
            if ((h0 & 0xf000) != 0x8000 || (h1 & 0xc000) != 0xc000)
                goto reset;
            b->tgt = b->pos + 2 * vc4_sext(h1 & 0xff, 8);
            break;
        default:
            goto reset;
        }
        if (b->tgt < fn_start || b->tgt >= end)
            goto reset;
    }

    /* optimistic start */
    for (i = 0; i < nb_brs; i++) {
        VC4Br *b = &brs[i];
        b->st = 0;
        if (b->kind != BR_B32)
            continue;
        b->st = 1;
        if (b->cc == VC4_AL || b->cmp < fn_start || b->ra > 15
            || (b->rb >= 0 ? b->rb > 15 : b->imm < 0 || b->imm > 63)
            || (i > 0 && b->cmp < vc4_br_end(&brs[i - 1])))
            continue;
        /* the flags must die here: a following b<cc> without its own
           compare (long long compares) still reads them */
        if (i + 1 < nb_brs && brs[i + 1].pos == b->pos + 4
            && brs[i + 1].kind == BR_B32 && brs[i + 1].cmp < 0
            && brs[i + 1].cc != VC4_AL)
            continue;
        for (j = 0; j < nb_brs; j++)   /* nothing may jump past the cmp */
            if (brs[j].tgt > b->cmp && brs[j].tgt <= b->pos)
                break;
        if (j == nb_brs)
            b->st = 2;
    }

    sav = tcc_malloc((nb_brs + 1) * sizeof *sav);
    do {
        changed = 0;
        for (i = sav[0] = 0; i < nb_brs; i++)
            sav[i + 1] = sav[i] + vc4_br_end(&brs[i]) - vc4_br_start(&brs[i])
                         - vc4_br_len(&brs[i]);
        for (i = 0; i < nb_brs; i++) {
            VC4Br *b = &brs[i];
            int at = vc4_relax_map(vc4_br_start(b), sav);
            int off = (vc4_relax_map(b->tgt, sav) - at) >> 1;
            int ok = b->kind == BR_B16 || (b->kind == BR_B32 && b->st == 1)
                         ? off >= -64 && off < 64
                   : b->kind == BR_CIMM || (b->st == 2 && b->rb < 0)
                         ? off >= -128 && off < 128
                   : b->kind == BR_CREG || b->st == 2
                         ? off >= -512 && off < 512
                   : 1;
            if (!ok) {
                if (b->kind != BR_B32)
                    goto fail;   /* cannot happen: distances only shrink */
                b->st--;
                changed = 1;
            }
        }
    } while (changed);

    /* targets and relocations must not fall inside an edited region */
    for (i = 0; i < nb_brs; i++) {
        int s = vc4_br_start(&brs[i]), e = vc4_br_end(&brs[i]);
        for (j = 0; j < nb_brs; j++)
            if (brs[j].tgt > s && brs[j].tgt < e)
                goto fail;
    }
    if (cur_text_section->reloc) {
        Section *sr = cur_text_section->reloc;
        for (rel = (ElfW_Rel *)(sr->data + fn_reloc);
             rel < (ElfW_Rel *)(sr->data + sr->data_offset); rel++)
            for (i = 0; i < nb_brs; i++)
                if (rel->r_offset >= (unsigned)vc4_br_start(&brs[i])
                    && rel->r_offset < (unsigned)vc4_br_end(&brs[i]))
                    goto fail;
    }

    /* rewrite */
    old = tcc_malloc(end - fn_start);
    memcpy(old, d + fn_start, end - fn_start);
    {
        int src = fn_start;
        ind = fn_start;
        for (i = 0; i < nb_brs; i++) {
            VC4Br *b = &brs[i];
            int s = vc4_br_start(b), at, off;
            memcpy(d + ind, old + (src - fn_start), s - src);
            ind += s - src;
            at = ind;
            off = vc4_relax_map(b->tgt, sav) - at;
            switch (b->kind) {
            case BR_B32:
                if (b->st == 2 && b->rb >= 0)
                    vc4_bcmp_reg(b->cc, b->ra, b->rb, off);
                else if (b->st == 2)
                    vc4_bcmp_imm(b->cc, b->ra, b->imm, off);
                else if (b->st == 1)
                    vc4_bcc16(b->cc, off);
                else
                    vc4_bcc32(b->cc, off);
                break;
            case BR_B16:
                vc4_bcc16(b->cc, off);
                break;
            case BR_CREG:
            case BR_CIMM: {
                unsigned h1 = read16le(old + (b->pos + 2 - fn_start));
                memcpy(d + ind, old + (b->pos - fn_start), 2);
                ind += 2;
                if (b->kind == BR_CREG)
                    h1 = (h1 & ~0x3ff) | ((off >> 1) & 0x3ff);
                else
                    h1 = (h1 & ~0xff) | ((off >> 1) & 0xff);
                VC4_EMIT16(h1);
                break;
            }
            case BR_DEL:
                break;
            }
            src = vc4_br_end(b);
        }
        memcpy(d + ind, old + (src - fn_start), end - src);
        ind += end - src;
    }
    tcc_free(old);
    if (cur_text_section->reloc) {
        Section *sr = cur_text_section->reloc;
        for (rel = (ElfW_Rel *)(sr->data + fn_reloc);
             rel < (ElfW_Rel *)(sr->data + sr->data_offset); rel++)
            if (rel->r_offset >= (unsigned)fn_start && rel->r_offset < (unsigned)end)
                rel->r_offset = vc4_relax_map(rel->r_offset, sav);
    }
fail:
    tcc_free(sav);
reset:
    nb_brs = 0;
    if (ind != end) {
        lc.end = li.end = la.end = st_end = -1;
        vc4_label_ind = ind;
    }
}

ST_FUNC void gfunc_epilog(void)
{
    int v = (-loc + 3) & -4, saved_ind, frame, del, i;

    vc4_inc_flush_sv(NULL);
    /* a function that never reads r7 has no frame: sp stays where the
       push left it, and vc4_relax() drops "add r7,sp,..; add sp,.." from
       the prologue (if it cannot, they stay as "add sp,0" and an r7 that
       nobody reads) */
    frame = fp_used || func_regsave;
    if (frame)
        vc4_add_imm(VC4_SP, FP, VC4_FP_BIAS);
    else
        v = 0;
    for (i = 0; i < nb_argrv; i++)
        reg_classes[argrv_reg[i]] |= RC_INT;
    nb_argrv = 0;
    del = regpush_ind + 2;
    if (fn_leaf && !frame && !nb_regvar && !r6_used && !nocode_wanted) {
        /* nothing to save: the push becomes a nop (dropped below) */
        write16le(cur_text_section->data + regpush_ind, 0x0001);
        vc4_rts();
        del = regpush_ind;
    } else if (func_regsave) {
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
    if (!frame && !nocode_wanted) {
        vc4_br_add(del, BR_DEL, 0);
        if (nb_brs && brs[nb_brs - 1].pos == del)
            brs[nb_brs - 1].imm = func_sub_sp_offset + 4 - del;
    }
    fn_leaf = 0;
    vc4_relax();
}

ST_FUNC void ggoto(void)
{
    vc4_inc_flush_sv(NULL);
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
            r6_used = 1;
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

    /* 0 - x: neg; -(x & 1): signext from bit 0 */
    if (vop == VC4_SUB && !vc4_const_op()
        && (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST
        && vtop[-1].c.i == 0) {
        int rs = vc4_regvar_of(vtop), pos, tmp = rs < 0;
        if (rs < 0)
            rs = gv(RC_INT);
        r = tmp ? rs : get_reg(RC_INT);
        if (tmp && la.end == ind && la.rd == rs && vc4_label_ind != ind
            && la.kind == LA_ALUI && la.c == 1
            && (la.op == VC4_AND || la.op == VC4_BMASK)) {
            int a = la.a;
            ind = pos = la.pos;
            vc4_alu32i(VC4_SIGNEXT, VC4_AL, r, a, 0);
            la_set(pos, r, LA_ALUI, VC4_SIGNEXT, a, 0, 0);
        } else {
            vc4_alu16(VC4_NEG, r, rs);
            la.end = -1;
        }
        vtop--;
        vtop->r = r;
        vtop->r2 = VT_CONST;
        return;
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
                int pos = ind;
                if (VC4_FITS_S(c, 6))
                    vc4_alu32i(VC4_CMP, VC4_AL, 0, lr, c);
                else if (VC4_FITS_S(c, 16))
                    vc4_alu32i16(VC4_CMP, lr, c);
                else
                    vc4_alu48(VC4_CMP, lr, c);
                lc_set_imm(pos, lr, c);
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
            if ((vop == VC4_LSR || vop == VC4_ASR) && c > 0 && la.end == ind
                && la.rd == r && la.kind == LA_ALUI && la.op == VC4_SHL
                && la.c == c && vc4_label_ind != ind) {
                /* (x << c) >> c: bmask (unsigned) or signext (signed) */
                int a = la.a, nop = vop == VC4_LSR ? VC4_BMASK : VC4_SIGNEXT;
                int n = vop == VC4_LSR ? 32 - c : 31 - c, pos = la.pos;
                ind = pos;
                if (a == r)
                    vc4_alu16i(nop, r, n);
                else
                    vc4_alu32i(nop, VC4_AL, r, a, n);
                la_set(pos, r, LA_ALUI, nop, a, 0, n);
                goto done;
            }
            {
                int pos = ind;
                vc4_alu16i(vop, r, c);
                la_set(pos, r, LA_ALUI, vop, r, 0, c);
                if (vop == VC4_CMP)
                    lc_set_imm(pos, r, c);
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
        {
            int pos = ind;
            if (vop == VC4_ADD)
                vc4_add_imm(r, r, c);
            else if (vop == VC4_CMP && VC4_FITS_S(c, 6))
                vc4_alu32i(VC4_CMP, VC4_AL, 0, r, c);
            else if (VC4_FITS_S(c, 16))
                vc4_alu32i16(vop, r, c);
            else
                vc4_alu48(vop, r, c);
            if (vop == VC4_CMP)
                lc_set_imm(pos, r, c);
        }
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
