#ifndef FICC_ELF_H
#define FICC_ELF_H

#include "dwarf.h"

/* ELF64 relocatable object writer; non-NULL `dwarf` adds CFI + debug sections. */
void elf_write(CodegenModule *cm, const char *path, const DwarfOutput *dwarf);

#endif