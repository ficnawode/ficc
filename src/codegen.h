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
    Vec *patches;   /* Vec<Patch*> — internal to codegen.c */
};

/* Module-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs; /* Vec<CodegenFunc*> */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena);

#endif
