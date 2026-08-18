#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "ir.h"
#include "util/bytebuf.h"
#include "util/types.h"

/* Patch record for a global-data reference.
   elf.c reads these to emit R_X86_64_32 relocations. */
typedef struct
{
    size_t offset;    /* byte offset of the immediate within the function's bytebuf */
    u32 global_index; /* index into IrModule globals */
} GlobalPatch;

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    ByteBuf *bytes;      /* machine-code bytes; the single source of truth for output */
    size_t offset;       /* start position in .text */
    Vec *patches;        /* Vec<CallPatch*> — function call patches */
    Vec *global_patches; /* Vec<GlobalPatch*> — global-data reference patches */
    bool is_static;      /* emit as STB_LOCAL in the object file */
};

/* IrModule-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs;   /* Vec<CodegenFunc*> */
    Vec *globals; /* Vec<IrGlobal*> — for .rodata/.data/.bss emission */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(IrModule *ir, Arena *arena);

#endif
