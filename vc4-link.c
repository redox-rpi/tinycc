/*
 *  VideoCore IV (VPU) relocations for TinyCC.
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Relocation numbers as the vc4-toolchain objects use them (the ELF ABI
 *  we must match to link with vc4-elf-ld). The relocation offset is the
 *  address of the instruction, not of the field inside it.
 */
#ifdef TARGET_DEFS_ONLY

#define EM_TCC_TARGET EM_VIDEOCORE3

/* relocation type for 32 bit data relocation */
#define R_DATA_32   R_VC4_32
#define R_DATA_PTR  R_VC4_32
#define R_JMP_SLOT  R_VC4_NONE
#define R_GLOB_DAT  R_VC4_NONE
#define R_COPY      R_VC4_NONE
#define R_RELATIVE  R_VC4_NONE

#define R_NUM       R_VC4_NUM

#define ELF_START_ADDR 0x80000000
#define ELF_PAGE_SIZE  0x1000

#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0

#else /* !TARGET_DEFS_ONLY */

#include "tcc.h"

/* Returns 1 for a code relocation, 0 for a data relocation. For unknown
   relocations, returns -1. */
ST_FUNC int code_reloc (int reloc_type)
{
    switch (reloc_type) {
    case R_VC4_32:
    case R_VC4_IMM32:
        return 0;
    case R_VC4_PCREL27_MUL2:
    case R_VC4_PCREL23_MUL2:
    case R_VC4_PCREL27:
        return 1;
    }
    return -1;
}

/* No GOT or PLT: the firmware is linked statically. */
ST_FUNC int gotplt_entry_type (int reloc_type)
{
    switch (reloc_type) {
    case R_VC4_32:
    case R_VC4_IMM32:
    case R_VC4_PCREL27_MUL2:
    case R_VC4_PCREL23_MUL2:
    case R_VC4_PCREL27:
        return NO_GOTPLT_ENTRY;
    }
    return -1;
}

ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
    tcc_error_noabort("VC4: no PLT");
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
    uint32_t off, h0;

    switch (type) {
    case R_VC4_32:
        write32le(ptr, val);
        return;
    case R_VC4_IMM32: /* 48-bit insn, imm32 in halfwords 1 and 2 */
        write16le(ptr + 2, val);
        write16le(ptr + 4, val >> 16);
        return;
    case R_VC4_PCREL27_MUL2: /* bl: 27-bit halfword offset */
        off = (uint32_t)(val - addr) >> 1;
        h0 = read16le(ptr) & 0xf080;
        write16le(ptr, h0 | ((off >> 23) & 0xf) << 8 | ((off >> 16) & 0x7f));
        write16le(ptr + 2, off);
        return;
    case R_VC4_PCREL23_MUL2: /* b<cc>: 23-bit halfword offset */
        off = (uint32_t)(val - addr) >> 1;
        h0 = read16le(ptr) & 0xff80;
        write16le(ptr, h0 | ((off >> 16) & 0x7f));
        write16le(ptr + 2, off);
        return;
    case R_VC4_PCREL27: /* 48-bit ld/st (pc + simm27) */
        off = (uint32_t)(val - addr) & 0x7ffffff;
        write16le(ptr + 2, off);
        write16le(ptr + 4, (read16le(ptr + 4) & 0xf800) | off >> 16);
        return;
    case R_VC4_NONE:
        return;
    }
    tcc_error_noabort("VC4: unhandled relocation %d at %x", type, (unsigned)addr);
}

#endif /* !TARGET_DEFS_ONLY */
