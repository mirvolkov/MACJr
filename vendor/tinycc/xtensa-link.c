/*
 * Xtensa relocations for TCC (MACJr).
 *
 * The code generator (xtensa-gen.c) only ever emits one kind of
 * relocation: R_XTENSA_32, a full 32-bit address word (an inline literal
 * that an L32R loads, or a pointer in initialized data). TCC itself only
 * produces relocatable objects for this target (ELF_OBJ_ONLY); the program
 * ELF that the MACJr loader runs is written by apps/cc/cc.c, which turns
 * every R_XTENSA_32 into R_XTENSA_RELATIVE (a symbol of the program) or
 * R_XTENSA_GLOB_DAT (a firmware symbol, looked up by name at load time).
 */
#ifdef TARGET_DEFS_ONLY

#define EM_TCC_TARGET EM_XTENSA

#define R_XTENSA_NONE     0
#define R_XTENSA_32       1
#define R_XTENSA_RTLD     2
#define R_XTENSA_GLOB_DAT 3
#define R_XTENSA_JMP_SLOT 4
#define R_XTENSA_RELATIVE 5
#define R_XTENSA_PLT      6

#define R_DATA_32  R_XTENSA_32
#define R_DATA_PTR R_XTENSA_32
#define R_JMP_SLOT R_XTENSA_JMP_SLOT
#define R_GLOB_DAT R_XTENSA_GLOB_DAT
#define R_COPY     R_XTENSA_NONE
#define R_RELATIVE R_XTENSA_RELATIVE

#define R_NUM      60

#define ELF_START_ADDR 0x00000000
#define ELF_PAGE_SIZE  0x1000

#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT   0

#else /* !TARGET_DEFS_ONLY */

#include "tcc.h"

/* Returns 1 for a code relocation, 0 for a data relocation, -1 if unknown. */
ST_FUNC int code_reloc(int reloc_type)
{
    return reloc_type == R_XTENSA_32 ? 0 : -1;
}

ST_FUNC int gotplt_entry_type(int reloc_type)
{
    return NO_GOTPLT_ENTRY;
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr,
                      addr_t addr, addr_t val)
{
    if (type == R_XTENSA_32) {
        write32le(ptr, val);
        return;
    }
    tcc_error_noabort("unsupported relocation type %d", type);
}

/* TCC's own linker is not used for this target (apps/cc/cc.c writes the
   program), so there is no PLT. */
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
    tcc_error_noabort("no PLT on xtensa");
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
}

#endif /* !TARGET_DEFS_ONLY */
