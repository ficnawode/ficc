#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "ir.h"
#include "util/bytebuf.h"
#include "util/types.h"

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    ByteBuf *bytes; /* machine-code bytes; the single source of truth for output */
    size_t offset;  /* start position in .text */
    Vec *patches;   /* Vec<Patch*> — function call patches */
    Vec *global_patches; /* Vec<GlobalPatch*> — internal to codegen.c */
};

/* Patch record for global-data references.
   Elm.c reads this to emit R_X86_64_32 relocations. */
typedef struct
{
    size_t offset;   /* byte offset of the immediate within the function's bytebuf */
    u32 global_index; /* index into IrModule globals / CodegenModule rodata_offsets */
} GlobalPatch;

/* IrModule-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs;         /* Vec<CodegenFunc*> */
    Vec *globals;       /* Vec<IrGlobal*> — for .rodata/.data emission */
    Vec *rodata_offsets; /* Vec<size_t> — byte offset of each global within .rodata */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(IrModule *ir, Arena *arena);

#endif
