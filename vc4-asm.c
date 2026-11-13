/*
 *  VideoCore IV (VPU) assembler for TinyCC: .S files and inline asm.
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Syntax as the vc4 GNU assembler takes it (black-box checked by
 *  tests/vc4/asm-test.sh); instructions come from vc4-enc.h. Covers what the
 *  firmware's bootcode needs and the common integer instructions:
 *
 *    nop rts rti di ei sleep bkpt      version rd
 *    mov rd, rs | #imm | symbol        (shortest form; symbols 48-bit)
 *    add sub mul and or eor bic shl lsr asr ror rsub min max
 *        rd, rs | rd, #imm | rd, ra, rb | rd, ra, #imm
 *    cmp cmn btest  ra, rb | ra, #imm   neg not abs msb brev  rd, rs
 *    ld st ldh sth ldb stb ldsh  rd, (ra) | (ra+off) | (ra-off)
 *    b bl  label | rN                  b<cc> label
 *
 *  Branches to a local label already defined in the same section are
 *  resolved here (16-bit form when it fits); all others get a relocation.
 *  The code shares one translation unit with vc4-gen.c (ONE_SOURCE) and
 *  uses its emitters and helpers.
 */

#ifdef TARGET_DEFS_ONLY

#define CONFIG_TCC_ASM
#define NB_ASM_REGS 32

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

#else

#define USING_GLOBALS
#include "tcc.h"

ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = c;
    ind = ind1;
}

ST_FUNC void gen_le16(int i)
{
    g(i);
    g(i >> 8);
}

ST_FUNC void gen_le32(int i)
{
    gen_le16(i);
    gen_le16(i >> 16);
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    if (pe->pcrel)
        tcc_error("VC4: pc-relative data not supported");
    if (pe->sym) {
        greloca(cur_text_section, pe->sym, ind, R_VC4_32, pe->v);
        gen_le32(0);
    } else {
        gen_le32(pe->v);
    }
}

/* ---- operands ---- */

enum { OP_REG, OP_IMM, OP_MEM };

typedef struct {
    int type;
    int reg;            /* OP_REG, base of OP_MEM */
    ExprValue e;        /* OP_IMM, offset of OP_MEM */
} VOperand;

/* register number of a name, or -1 */
static int vc4_reg_of_name(const char *s)
{
    int n;
    char *end;
    if (!strcmp(s, "sp")) return VC4_SP;
    if (!strcmp(s, "lr")) return VC4_LR;
    if (!strcmp(s, "gp")) return VC4_GP;
    if (!strcmp(s, "sr")) return VC4_SR;
    if (!strcmp(s, "pc")) return VC4_PC;
    if (s[0] != 'r' || s[1] < '0' || s[1] > '9')
        return -1;
    n = strtol(s + 1, &end, 10);
    if (*end || n > 31)
        return -1;
    return n;
}

ST_FUNC int asm_parse_regvar(int t)
{
    if (t < TOK_IDENT)
        return -1;
    return vc4_reg_of_name(get_tok_str(t, NULL));
}

static void vc4_parse_operand(TCCState *s1, VOperand *op)
{
    int r;

    if (tok == '(') {
        next();
        if ((r = asm_parse_regvar(tok)) < 0) {
            unget_tok('(');     /* an expression in parentheses */
            goto expr;
        }
        next();
        op->type = OP_MEM;
        op->reg = r;
        op->e.v = 0;
        op->e.sym = NULL;
        op->e.pcrel = 0;
        if (tok == '+' || tok == '-') {
            int neg = tok == '-';
            next();
            asm_expr(s1, &op->e);
            if (op->e.sym)
                expect("constant offset");
            if (neg)
                op->e.v = -op->e.v;
        }
        skip(')');
        return;
    }
    if ((r = asm_parse_regvar(tok)) >= 0) {
        next();
        op->type = OP_REG;
        op->reg = r;
        return;
    }
    if (tok == '#')
        next();
  expr:
    asm_expr(s1, &op->e);
    op->type = OP_IMM;
}

static int vc4_parse_operands(TCCState *s1, VOperand *ops, int max)
{
    int n = 0;
    if (tok == ';' || tok == TOK_LINEFEED || tok == TOK_EOF)
        return 0;
    for (;;) {
        if (n == max)
            tcc_error("too many operands");
        vc4_parse_operand(s1, &ops[n++]);
        if (tok != ',')
            break;
        next();
    }
    return n;
}

static int vc4_imm_const(VOperand *op)
{
    if (op->type != OP_IMM)
        expect("immediate");
    if (op->e.sym)
        expect("constant");
    return (int)op->e.v;
}

static int vc4_reg_op(VOperand *op)
{
    if (op->type != OP_REG)
        expect("register");
    return op->reg;
}

/* ---- instruction tables ---- */

static const struct { const char *name; int op; } vc4_alu_names[] = {
    { "mov", VC4_MOV }, { "add", VC4_ADD }, { "sub", VC4_SUB },
    { "mul", VC4_MUL }, { "and", VC4_AND }, { "or", VC4_OR },
    { "eor", VC4_EOR }, { "bic", VC4_BIC }, { "shl", VC4_SHL },
    { "lsr", VC4_LSR }, { "asr", VC4_ASR }, { "ror", VC4_ROR },
    { "rsub", VC4_RSUB }, { "min", VC4_MIN }, { "max", VC4_MAX },
    { "cmp", VC4_CMP }, { "cmn", VC4_CMN }, { "btest", VC4_BTEST },
    { "neg", VC4_NEG }, { "not", VC4_NOT }, { "abs", VC4_ABS },
    { "msb", VC4_MSB }, { "brev", VC4_BREV },
};

static const struct { const char *name; int sz; } vc4_ldst_names[] = {
    { "ld", VC4_LD }, { "st", VC4_ST }, { "ldh", VC4_LDH },
    { "sth", VC4_STH }, { "ldb", VC4_LDB }, { "stb", VC4_STB },
    { "ldsh", VC4_LDSH },
};

static const char vc4_cc_names[][3] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le",
};

static int vc4_parse_cc(const char *s)
{
    int i;
    if (!strcmp(s, "lo")) return VC4_LO;
    if (!strcmp(s, "hs")) return VC4_HS;
    for (i = 0; i < 14; i++)
        if (!strcmp(s, vc4_cc_names[i]))
            return i;
    return -1;
}

/* the 2-operand forms whose first source is not the destination */
static int vc4_is_unary(int op)
{
    return op == VC4_MOV || op == VC4_NEG || op == VC4_NOT || op == VC4_ABS
        || op == VC4_MSB || op == VC4_BREV;
}

static int vc4_is_compare(int op)
{
    return op == VC4_CMP || op == VC4_CMN || op == VC4_BTEST;
}

/* ---- emitters ---- */

static void vc4_asm_alu(TCCState *s1, int op)
{
    VOperand ops[3];
    int n = vc4_parse_operands(s1, ops, 3);
    int rd, ra, v;

    if (n < 2)
        expect("two or three operands");
    rd = vc4_reg_op(&ops[0]);
    if (n == 3) {
        if (vc4_is_unary(op) || vc4_is_compare(op))
            tcc_error("three operands not allowed here");
        ra = vc4_reg_op(&ops[1]);
        if (ops[2].type == OP_REG) {
            vc4_alu32(op, VC4_AL, rd, ra, ops[2].reg);
        } else {
            v = vc4_imm_const(&ops[2]);
            if (VC4_FITS_S(v, 6))
                vc4_alu32i(op, VC4_AL, rd, ra, v);
            else if (op == VC4_ADD)
                vc4_add48(rd, ra, v);
            else
                tcc_error("immediate %d out of range", v);
        }
        return;
    }
    if (ops[1].type == OP_REG) {
        int rs = ops[1].reg;
        if (op == VC4_MOV && rd != rs) {
            vc4_mov_reg(rd, rs);
        } else if (VC4_FAST(rd) && VC4_FAST(rs)) {
            vc4_alu16(op, rd, rs);
        } else if (vc4_is_compare(op)) {
            vc4_alu32(op, VC4_AL, 0, rd, rs);
        } else {
            vc4_alu32(op, VC4_AL, rd, vc4_is_unary(op) ? 0 : rd, rs);
        }
        return;
    }
    if (ops[1].type != OP_IMM)
        expect("register or immediate");
    if (ops[1].e.sym) {
        if (op != VC4_MOV)
            expect("constant");
        greloca(cur_text_section, ops[1].e.sym, ind, R_VC4_IMM32, ops[1].e.v);
        vc4_mov48(rd, 0);
        return;
    }
    v = ops[1].e.v;
    if (op == VC4_MOV) {
        if (!(VC4_FAST(rd) && v >= 0 && v < 32) && VC4_FITS_S(v, 6))
            vc4_alu32i(VC4_MOV, VC4_AL, rd, 0, v);  /* as gas picks */
        else
            vc4_mov_imm(rd, v);
    } else if (VC4_FAST(rd) && v >= 0 && v < 32 && vc4_op4[op] >= 0) {
        vc4_alu16i(op, rd, v);
    } else if (vc4_is_unary(op)) {
        tcc_error("immediate not allowed here");
    } else if (VC4_FITS_S(v, 16)) {
        vc4_alu32i16(op, rd, v);
    } else if (op == VC4_ADD) {
        vc4_add48(rd, rd, v);
    } else {
        tcc_error("immediate %d out of range", v);
    }
}

static void vc4_asm_ldst(TCCState *s1, int sz)
{
    VOperand ops[2];
    int rd;

    if (vc4_parse_operands(s1, ops, 2) != 2 || ops[1].type != OP_MEM)
        expect("rd, (ra[+offset])");
    rd = vc4_reg_op(&ops[0]);
    vc4_ldst(sz, rd, ops[1].reg, (int)ops[1].e.v);
}

/* branch target: offset from the instruction if the label is already
   defined in this section, else -1 with *sym set */
static int vc4_branch_target(TCCState *s1, VOperand *op, int *off, Sym **sym)
{
    ElfSym *esym;

    if (op->type != OP_IMM || op->e.pcrel)
        expect("label");
    *sym = op->e.sym;
    if (!op->e.sym) {
        *off = (int)op->e.v - ind;      /* absolute address in this section */
        return 0;
    }
    esym = elfsym(op->e.sym);
    /* as gas: only local labels are resolved here, global symbols keep
       their relocation */
    if (esym && esym->st_shndx == cur_text_section->sh_num
        && ELFW(ST_BIND)(esym->st_info) == STB_LOCAL) {
        *off = (int)(esym->st_value + op->e.v) - ind;
        return 0;
    }
    return -1;
}

static void vc4_asm_branch(TCCState *s1, int cc, int link)
{
    VOperand op;
    Sym *sym;
    int off;

    vc4_parse_operand(s1, &op);
    if (op.type == OP_REG) {
        if (cc != VC4_AL)
            tcc_error("conditional branch to a register not supported");
        if (link)
            vc4_bl_reg(op.reg);
        else
            vc4_b_reg(op.reg);
        return;
    }
    if (vc4_branch_target(s1, &op, &off, &sym) == 0) {
        if (off & 1)
            tcc_error("branch target not 16-bit aligned");
        if (link)
            vc4_bl32(off);
        else if (off >= -128 && off < 128)
            vc4_bcc16(cc, off);
        else
            vc4_bcc32(cc, off);
        return;
    }
    if (link) {
        greloca(cur_text_section, sym, ind, R_VC4_PCREL27_MUL2, op.e.v);
        vc4_bl32(0);
    } else {
        greloca(cur_text_section, sym, ind, R_VC4_PCREL23_MUL2, op.e.v);
        vc4_bcc32(cc, 0);
    }
}

ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    const char *s = get_tok_str(opcode, NULL);
    unsigned i;
    int cc;

    if (!strcmp(s, "nop")) { vc4_nop(); return; }
    if (!strcmp(s, "rts")) { vc4_rts(); return; }
    if (!strcmp(s, "rti")) { o16(0x000a); return; }
    if (!strcmp(s, "di")) { o16(0x0005); return; }
    if (!strcmp(s, "ei")) { o16(0x0004); return; }
    if (!strcmp(s, "sleep")) { o16(0x0002); return; }
    if (!strcmp(s, "bkpt")) { o16(0x0000); return; }
    if (!strcmp(s, "version")) {
        VOperand op;
        vc4_parse_operand(s1, &op);
        o16(0x00e0 | vc4_reg_op(&op));
        return;
    }
    for (i = 0; i < sizeof vc4_alu_names / sizeof *vc4_alu_names; i++)
        if (!strcmp(s, vc4_alu_names[i].name)) {
            vc4_asm_alu(s1, vc4_alu_names[i].op);
            return;
        }
    for (i = 0; i < sizeof vc4_ldst_names / sizeof *vc4_ldst_names; i++)
        if (!strcmp(s, vc4_ldst_names[i].name)) {
            vc4_asm_ldst(s1, vc4_ldst_names[i].sz);
            return;
        }
    if (!strcmp(s, "b")) { vc4_asm_branch(s1, VC4_AL, 0); return; }
    if (!strcmp(s, "bl")) { vc4_asm_branch(s1, VC4_AL, 1); return; }
    if (s[0] == 'b' && strlen(s) == 3 && (cc = vc4_parse_cc(s + 1)) >= 0) {
        vc4_asm_branch(s1, cc, 0);
        return;
    }
    tcc_error("VC4: unknown or unsupported instruction '%s'", s);
}

/* ---- inline asm ---- */

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    int r = sv->r, reg;

    if ((r & VT_VALMASK) == VT_CONST) {
        if (r & VT_SYM) {
            const char *name = get_tok_str(sv->sym->v, NULL);
            if (sv->sym->v >= SYM_FIRST_ANOM)
                get_asm_sym(tok_alloc(name, strlen(name))->tok, sv->sym);
            cstr_cat(add_str, name, -1);
            if ((int)sv->c.i == 0)
                return;
            cstr_ccat(add_str, '+');
        }
        cstr_printf(add_str, "%d", modifier == 'n' ? -(int)sv->c.i : (int)sv->c.i);
    } else if ((r & VT_VALMASK) == VT_LOCAL) {
        cstr_printf(add_str, "(r%d%+d)", FP, (int)sv->c.i + VC4_FP_BIAS);
    } else {
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            tcc_internal_error("");
        if (r & VT_LVAL)
            cstr_printf(add_str, "(r%d)", reg);
        else
            cstr_printf(add_str, "r%d", reg);
    }
}

/* r6 is scratch and r0-r5 are caller-saved; an asm that clobbers r7 (the
   frame pointer) up to r23 gets those saved around it */
#define VC4_ASM_SAVE_FIRST 7
#define VC4_ASM_SAVE_LAST 23

ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands,
                          int nb_outputs, int is_output,
                          uint8_t *clobber_regs, int out_reg)
{
    ASMOperand *op;
    int i, reg;

    vc4_label_ind = ind;   /* no store-to-load forwarding across asm */
    if (!is_output) {
        for (reg = VC4_ASM_SAVE_FIRST; reg <= VC4_ASM_SAVE_LAST; reg++)
            if (clobber_regs[reg]) {
                vc4_add_imm(VC4_SP, VC4_SP, -4);
                vc4_ldst(VC4_ST, reg, VC4_SP, 0);
            }
        for (i = 0; i < nb_operands; i++) {
            op = &operands[i];
            if (op->reg < 0)
                continue;
            if ((op->vt->r & VT_VALMASK) == VT_LLOCAL && op->is_memory) {
                SValue sv = *op->vt;
                sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
                sv.type.t = VT_PTR;
                load(op->reg, &sv);
            } else if (i >= nb_outputs || op->is_rw) {
                load(op->reg, op->vt);
            }
        }
    } else {
        for (i = 0; i < nb_outputs; i++) {
            op = &operands[i];
            if (op->reg < 0)
                continue;
            if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                if (!op->is_memory) {
                    SValue sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                    sv.type.t = VT_PTR;
                    load(out_reg, &sv);
                    sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | out_reg;
                    store(op->reg, &sv);
                }
            } else {
                store(op->reg, op->vt);
            }
        }
        for (reg = VC4_ASM_SAVE_LAST; reg >= VC4_ASM_SAVE_FIRST; reg--)
            if (clobber_regs[reg]) {
                vc4_ldst(VC4_LD, reg, VC4_SP, 0);
                vc4_add_imm(VC4_SP, VC4_SP, 4);
            }
    }
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK  0x02
#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

static int vc4_constraint_priority(const char *str)
{
    int priority = 0, pr;
    for (; *str; str++) {
        switch (*str) {
        case 'r': pr = 3; break;
        case 'i': case 'n': case 'I': case 'm': case 'g': pr = 4; break;
        case '=': case '+': case '&': case '%': continue;
        default:
            tcc_error("unknown constraint '%c'", *str);
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands,
                                     int nb_outputs,
                                     const uint8_t *clobber_regs,
                                     int *pout_reg)
{
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];

    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
    }
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        str = op->constraint;
        while (*str == '=' || *str == '&' || *str == '+' || *str == '%')
            str++;
        if (isnum(*str) || *str == '[') {
            k = find_constraint(operands, nb_operands, str, NULL);
            if ((unsigned)k >= (unsigned)i || i < nb_outputs)
                tcc_error("invalid reference in constraint %d ('%s')", i, str);
            op->ref_index = k;
            if (operands[k].input_index >= 0)
                tcc_error("cannot reference twice the same operand");
            operands[k].input_index = i;
            op->priority = 5;
        } else if ((op->vt->r & VT_VALMASK) == VT_LOCAL && op->vt->sym
                   && (reg = op->vt->sym->r & VT_VALMASK) < VT_CONST) {
            op->priority = 1;
            op->reg = reg;
        } else {
            op->priority = vc4_constraint_priority(str);
        }
    }

    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++)
        for (j = i + 1; j < nb_operands; j++)
            if (operands[sorted_op[j]].priority < operands[sorted_op[i]].priority) {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }

    for (i = 0; i < NB_ASM_REGS; i++)
        regs_allocated[i] = clobber_regs[i] ? REG_IN_MASK | REG_OUT_MASK : 0;

    for (i = 0; i < nb_operands; i++) {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        if (op->ref_index >= 0)
            continue;
        if (op->input_index >= 0)
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        else if (j < nb_outputs)
            reg_mask = REG_OUT_MASK;
        else
            reg_mask = REG_IN_MASK;
        if (op->reg >= 0) {
            if (is_reg_allocated(op->reg))
                tcc_error("asm regvar requests register that's taken already");
            reg = op->reg;
        }
      try_next:
        c = *str++;
        switch (c) {
        case '=':
            goto try_next;
        case '+':
            op->is_rw = 1;
            /* fall through */
        case '&':
            if (j >= nb_outputs)
                tcc_error("'%c' modifier can only be applied to outputs", c);
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;
        case 'r':
            if ((reg = op->reg) >= 0)
                goto reg_found;
            for (reg = 0; reg < 6; reg++)
                if (!is_reg_allocated(reg))
                    goto reg_found;
            goto try_next;
          reg_found:
            op->is_llong = 0;
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;
        case 'i': case 'n': case 'I':
            if ((op->vt->r & (VT_VALMASK | VT_LVAL)) != VT_CONST)
                goto try_next;
            break;
        case 'm': case 'g':
            if (j < nb_outputs || c == 'm') {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL) {
                    for (reg = 0; reg < 6; reg++)
                        if (!(regs_allocated[reg] & REG_IN_MASK))
                            goto reg_found1;
                    goto try_next;
                  reg_found1:
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            }
            break;
        default:
            tcc_error("asm constraint %d ('%s') could not be satisfied",
                      j, op->constraint);
        }
        if (op->input_index >= 0) {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_llong = op->is_llong;
        }
    }

    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++) {
        op = &operands[i];
        if (op->reg >= 0 && (op->vt->r & VT_VALMASK) == VT_LLOCAL
            && !op->is_memory) {
            for (reg = 0; reg < 6; reg++)
                if (!(regs_allocated[reg] & REG_OUT_MASK))
                    goto reg_found2;
            tcc_error("could not find free output register for reloading");
          reg_found2:
            *pout_reg = reg;
            break;
        }
    }
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    int reg;

    if (!strcmp(str, "memory") || !strcmp(str, "cc"))
        return;
    reg = vc4_reg_of_name(str);
    if (reg < 0)
        tcc_error("invalid clobber register '%s'", str);
    clobber_regs[reg] = 1;
}

#endif /* TARGET_DEFS_ONLY */
