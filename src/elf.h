#ifndef FICC_ELF_H
#define FICC_ELF_H

#include "dwarf.h"
#include "elfdefs.h"
#include "util/arena.h"
#include "util/bytebuf.h"

/* Serialize a codegen module to an in-memory ELF64 relocatable object; the
   buffer is allocated from `arena`. Non-NULL `dwarf` adds CFI + debug sections. */
ByteBuf *elf_serialize(CodegenModule *cm, const DwarfOutput *dwarf, Arena *arena);

/* File wrapper over elf_serialize. */
void elf_write(CodegenModule *cm, const char *path, const DwarfOutput *dwarf);

#endif
