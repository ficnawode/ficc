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

/* A call to a function not defined in this module (a declaration-only extern,
   Phase 16). The direct `call rel32` stays unresolved (field = 0); elf.c emits
   an SHN_UNDEF symbol + an R_X86_64_PLT32 relocation so the linker resolves it
   (libc functions, other translation units). */
typedef struct
{
    const char *name;
    size_t text_offset; /* absolute byte offset of the rel32 field within .text */
} ExternCall;

/* Taking the *address* of a function (`&f`, or `f` as a designator value,
   D16.1): `mov $f, imm32sx` with an R_X86_64_32S relocation against the
   function symbol (defined in-module or SHN_UNDEF extern). */
typedef struct
{
    const char *name;
    size_t offset; /* byte offset of the immediate within the function's bytebuf */
} FuncAddrPatch;

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    ByteBuf *bytes;      /* machine-code bytes; the single source of truth for output */
    size_t offset;       /* start position in .text */
    Vec *patches;        /* Vec<CallPatch*> — function call patches */
    Vec *global_patches; /* Vec<GlobalPatch*> — global-data reference patches */
    Vec *func_patches;   /* Vec<FuncAddrPatch*> — function-address loads (D16.1) */
    bool is_static;      /* emit as STB_LOCAL in the object file */
};

/* IrModule-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs;        /* Vec<CodegenFunc*> */
    Vec *globals;      /* Vec<IrGlobal*> — for .rodata/.data/.bss emission */
    Vec *extern_calls; /* Vec<ExternCall*> — calls to declaration-only externs */
};

/* Convert IR to machine code bytes. */
CodegenModule *codegen_ir_to_machine(IrModule *ir, Arena *arena);

#endif
