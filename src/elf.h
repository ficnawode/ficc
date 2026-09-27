#ifndef FICC_ELF_H
#define FICC_ELF_H

#include "dwarf.h"
#include "elfdefs.h"
#include "util/arena.h"
#include "util/bytebuf.h"

ByteBuf *elf_serialize(CodegenModule *cm, const DwarfOutput *dwarf, Arena *arena);

void elf_write(CodegenModule *cm, const char *path, const DwarfOutput *dwarf);

#endif
