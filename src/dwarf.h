#ifndef FICC_DWARF_H
#define FICC_DWARF_H

#include "cfi.h"
#include "codegen.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "util/vec.h"

/* R_X86_64_64 reloc against the .text section symbol. */
typedef struct
{
    u64 offset;
    i64 addend;
} DwarfReloc;

/* Debug-section content plus the relocation lists elf_write consumes. */
typedef struct
{
    CfiOutput *cfi;
    ByteBuf debug_info;
    ByteBuf debug_abbrev;
    ByteBuf debug_str;
    ByteBuf debug_line;
    Vec *rela_info; /* Vec<DwarfReloc*> */
    Vec *rela_line; /* Vec<DwarfReloc*> */
} DwarfOutput;

/* unsigned/signed LEB128 encoders; exported for unit tests. */
void dwarf_uleb128(ByteBuf *b, u64 val);
void dwarf_sleb128(ByteBuf *b, i64 val);

/* Build .debug_line (+ .rela.debug_line) and a compile-unit DIE. */
void dwarf_build(CodegenModule *cm, const char *compile_unit, const char *comp_dir,
                 DwarfOutput *out, Arena *arena);

#endif