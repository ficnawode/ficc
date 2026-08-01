#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "ir.h"
#include "util/sbuf.h"
#include "util/types.h"

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc {
    const char *name;
    u8 *bytes;
    size_t len;
    size_t cap;
    size_t offset; /* start position in .text */
    Vec *patches;  /* Vec<CallPatch*> — internal to codegen.c */
};

/* Module-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule {
    Module *ir;   /* kept for text emission */
    Vec *funcs;   /* Vec<CodegenFunc*> */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena);

/* Debug: emit human-readable text assembly to Sbuf.
   Walks the original IR and calls the text emitter,
   so it cannot drift from the machine-code path. */
void codegen_text_dump(CodegenModule *cm, Sbuf *out);

#endif
