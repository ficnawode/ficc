#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "ir.h"
#include "util/sbuf.h"
#include "util/types.h"

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    u8 *bytes;
    size_t len;
    size_t cap;
    size_t offset; /* start position in .text */
    Vec *patches;  /* Vec<CallPatch*> — internal to codegen.c */
};

/* Module-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Module *ir; /* kept for text emission */
    Vec *funcs; /* Vec<CodegenFunc*> */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena);

/* Debug: dump human-readable x86 assembly to Sbuf.
   NOTE: this is a separate hand-written text emitter (emit_*_text in
   codegen.c), NOT a disassembly of the emitted machine bytes, so it is free
   to drift from the machine-code path. Keep the two in sync when adding an
   opcode; the -S dump is a debugging aid, not the single source of truth.
   TODO: fold this into a disassembler over CodegenFunc.bytes so the machine
   bytes become the one source of truth. */
void codegen_text_dump(CodegenModule *cm, Sbuf *out);

#endif
