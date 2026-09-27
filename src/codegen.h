#ifndef FICC_CODEGEN_H
#define FICC_CODEGEN_H

#include "cli.h"
#include "ir.h"
#include "regalloc.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "x86_emit.h"

typedef struct
{
    const char *name;
    size_t text_offset;
} ExternCall;

typedef struct
{
    size_t offset;
    u32 line;
} LineEntry;

typedef struct
{
    u32 off_push;
    u32 off_mov;
    u32 off_sub;
    u32 off_params;
    u8 saved_regs[16];
    u8 nsaved;
} FuncFrame;

typedef struct CodegenFunc CodegenFunc;
struct CodegenFunc
{
    const char *name;
    ByteBuf *bytes;
    size_t offset;
    Vec *patches;
    Vec *global_patches;
    Vec *func_patches;
    Vec *lines;
    FuncFrame frame;
    bool is_static;
    IrFunction *func;
    u32 *param_stage;
    const RegAllocation *alloc;
    u32 *position_offsets;
};

typedef struct CodegenModule CodegenModule;
struct CodegenModule
{
    Vec *funcs;
    Vec *globals;
    Vec *extern_calls;
};

CodegenModule *codegen_ir_to_machine(IrModule *ir, const CodegenConfig *cfg, Arena *arena);

u64 *codegen_global_offsets(CodegenModule *cm, ByteBuf *rodata, ByteBuf *data, ByteBuf *init_array,
                            ByteBuf *fini_array, Arena *arena);

#endif
