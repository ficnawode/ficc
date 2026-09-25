#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "cli.h"
#include "ir.h"
#include "regalloc.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "x86_emit.h"

/* Call to a declaration-only extern: rel32 field stays 0; elf.c emits an SHN_UNDEF symbol +
 * R_X86_64_PLT32 relocation. */
typedef struct
{
    const char *name;
    size_t text_offset; /* absolute byte offset of the rel32 field within .text */
} ExternCall;

/* Line boundary: `line` starts at byte `offset`; only recorded with -g. */
typedef struct
{
    size_t offset; /* byte offset within the function's bytes */
    u32 line;
} LineEntry;

/* Measured prologue byte offsets the CFI writer follows. */
typedef struct
{
    u32 off_push;      /* byte offset just past `push rbp` (0 if the frame omits the push) */
    u32 off_mov;       /* byte offset just past `mov rbp, rsp` */
    u32 off_sub;       /* byte offset just past `sub rsp, N` (== prologue end) */
    u32 off_params;    /* byte offset where the body starts, past the parameter homes */
    u8 saved_regs[16]; /* callee-saved pushes between off_mov and off_sub, in order */
    u8 nsaved;
} FuncFrame;

/* Per-function machine code record */
typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    ByteBuf *bytes;      /* machine-code bytes; the single source of truth for output */
    size_t offset;       /* start position in .text */
    Vec *patches;        /* Vec<PatchSite*> — function call patches */
    Vec *global_patches; /* Vec<GlobalPatch*> — global-data reference patches */
    Vec *func_patches;   /* Vec<FuncAddrPatch*> — function-address loads */
    Vec *lines;          /* Vec<LineEntry*>, NULL without -g */
    FuncFrame frame;     /* prologue layout for .eh_frame CFI */
    bool is_static;      /* emit as STB_LOCAL in the object file */
    IrFunction *func;    /* owning IR function: param names/types for .debug_info */
    u32 *slot_off;       /* per-vreg frame offsets below %rbp; spilled param locations */
    int *phys_map;       /* per-vreg physical register id, -1 when spilled; param locations */
    u32 *live_end;       /* per-vreg byte offset just past the value's last use (debug_loc) */
    u32 *param_stage;    /* per-param stage-slot offset below %rbp, 0 when there is none */
    const RegAllocation *alloc; /* segment table backing loc_at and debug locations */
    u32 *position_offsets;      /* position → byte offset, NULL without -g */
};

/* IrModule-level codegen records */
typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs;        /* Vec<CodegenFunc*> */
    Vec *globals;      /* Vec<IrGlobal*> — for .rodata/.data/.bss emission */
    Vec *extern_calls; /* Vec<ExternCall*> — calls to declaration-only externs */
};

CodegenModule *codegen_ir_to_machine(IrModule *ir, const CodegenConfig *cfg, Arena *arena);

/* Per-global .rodata/.data/.bss/.init_array/.fini_array offsets; shared by
   elf.c and dwarf.c layout. */
u64 *codegen_global_offsets(CodegenModule *cm, ByteBuf *rodata, ByteBuf *data, ByteBuf *init_array,
                            ByteBuf *fini_array, Arena *arena);

#endif
