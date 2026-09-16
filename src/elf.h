#ifndef FICC_ELF_H
#define FICC_ELF_H

#include "cfi.h"
#include "codegen.h"

/* ELF64 relocatable object writer; non-NULL `cfi` adds .eh_frame sections. */
void elf_write(CodegenModule *cm, const char *path, const CfiOutput *cfi);

#endif