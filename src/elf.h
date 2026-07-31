#ifndef FICC_ELF_H
#define FICC_ELF_H

#include "codegen.h"

/* Write a CodegenModule as an ELF64 relocatable object file. */
void elf_write(CodegenModule *cm, const char *path);

#endif
