#ifndef FICC_CFI_H
#define FICC_CFI_H

#include "codegen.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "util/vec.h"

/* R_X86_64_64 reloc: .text section symbol + the function's .text offset. */
typedef struct
{
    u64 offset; /* byte offset of the address slot within .eh_frame */
    i64 addend; /* .text-relative function offset */
} CfiReloc;

/* .eh_frame content plus the relocation list elf_write consumes. */
typedef struct
{
    ByteBuf eh_frame;
    Vec *relocs; /* Vec<CfiReloc*> — one per function's initial_location slot */
} CfiOutput;

/* Build a CIE and one FDE per function from the prologue layout. */
CfiOutput *cfi_build(CodegenModule *cm, Arena *arena);

#endif