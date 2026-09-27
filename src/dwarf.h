#ifndef FICC_DWARF_H
#define FICC_DWARF_H

#include "cfi.h"
#include "codegen.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct
{
    u64 offset;
    i64 addend;
    u32 sym;
} DwarfReloc;

enum
{
    DWARF_SYM_TEXT = 1,
    DWARF_SYM_RODATA = 2,
    DWARF_SYM_DATA = 3,
    DWARF_SYM_BSS = 4,
};

typedef struct
{
    CfiOutput *cfi;
    ByteBuf debug_info;
    ByteBuf debug_abbrev;
    ByteBuf debug_str;
    ByteBuf debug_line;
    ByteBuf debug_loc;
    Vec *rela_info;
    Vec *rela_line;
} DwarfOutput;

void dwarf_uleb128(ByteBuf *b, u64 val);
void dwarf_sleb128(ByteBuf *b, i64 val);

void dwarf_build(CodegenModule *cm, const char *compile_unit, const char *comp_dir,
                 DwarfOutput *out, Arena *arena);

#endif