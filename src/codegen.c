#include "codegen.h"
#include "util/bytebuf.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "x86_lower.h"
#include <stdint.h>

static u64 align_up(u64 n, u64 a)
{
    return (n + a - 1) / a * a;
}

static void assign_func_offsets(CodegenModule *cm, size_t nfuncs)
{
    size_t function_offset = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        cf->offset = function_offset;
        function_offset += bytebuf_len(cf->bytes);
    }
}

/* Resolve direct calls by name; undefined targets become SHN_UNDEF + R_X86_64_PLT32 in elf.c. */
static void resolve_direct_calls(CodegenModule *cm, size_t nfuncs, Arena *arena)
{
    StrMap *func_by_name = strmap_new(arena);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        strmap_set(func_by_name, cf->name, cf);
    }
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            PatchSite *site = (PatchSite *) vec_get(cf->patches, pi);
            CodegenFunc *target = (CodegenFunc *) strmap_get(func_by_name, site->target);
            if (!target)
            {
                ExternCall *ec = arena_alloc(arena, sizeof(ExternCall), sizeof(void *));
                ec->name = site->target;
                ec->text_offset = cf->offset + site->offset;
                vec_push(cm->extern_calls, ec);
                continue;
            }
            i32 rel = (i32) (target->offset - (cf->offset + site->offset + 4));
            bytebuf_poke_u32(cf->bytes, site->offset, (u32) rel);
        }
    }
}

CodegenModule *codegen_ir_to_machine(IrModule *ir, const CodegenConfig *cfg, Arena *arena)
{
    bool debug = cfg && cfg->debug;
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->funcs = vec_new(arena);
    cm->globals = ir->globals;
    cm->extern_calls = vec_new(arena);

    size_t nfuncs = x86_lower_module(cm, ir, debug, arena);
    assign_func_offsets(cm, nfuncs);
    resolve_direct_calls(cm, nfuncs, arena);
    return cm;
}

static u64 append_global(ByteBuf *buf, IrGlobal *g)
{
    u64 off = align_up(bytebuf_len(buf), g->align);
    while ((u64) bytebuf_len(buf) < off)
    {
        bytebuf_append(buf, 0);
    }
    if (g->init_data)
    {
        bytebuf_append_bytes(buf, g->init_data, g->init_len);
    }
    return off;
}

u64 *codegen_global_offsets(CodegenModule *cm, ByteBuf *rodata, ByteBuf *data, ByteBuf *init_array,
                            ByteBuf *fini_array, Arena *arena)
{
    size_t nglobals = cm->globals ? vec_size(cm->globals) : 0;
    u64 *global_off = arena_alloc(arena, (nglobals ? nglobals : 1) * sizeof(u64), sizeof(u64));
    u64 bss_size = 0;
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        switch (g->section)
        {
            case IR_SECTION_RODATA:
                global_off[i] = append_global(rodata, g);
                break;
            case IR_SECTION_DATA:
                global_off[i] = append_global(data, g);
                break;
            case IR_SECTION_INIT_ARRAY:
                global_off[i] = append_global(init_array, g);
                break;
            case IR_SECTION_FINI_ARRAY:
                global_off[i] = append_global(fini_array, g);
                break;
            case IR_SECTION_BSS:
                global_off[i] = align_up(bss_size, g->align);
                bss_size = global_off[i] + type_sizeof(g->type);
                break;
        }
    }
    return global_off;
}
