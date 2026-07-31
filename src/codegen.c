#include "codegen.h"
#include "util/assert.h"
#include <stdint.h>
#include <string.h>

typedef struct ByteBuf ByteBuf;
struct ByteBuf
{
    u8 *data;
    size_t len;
    size_t cap;
    Arena *arena;
};

static void bb_init(ByteBuf *bb, Arena *arena)
{
    bb->arena = arena;
    bb->len = 0;
    bb->cap = 64;
    bb->data = arena_alloc(arena, bb->cap, 1);
}

static void bb_grow(ByteBuf *bb, size_t need)
{
    if (bb->len + need > bb->cap)
    {
        while (bb->len + need > bb->cap)
            bb->cap *= 2;
        u8 *old = bb->data;
        bb->data = arena_alloc(bb->arena, bb->cap, 1);
        memcpy(bb->data, old, bb->len);
    }
}

static void bb_append(ByteBuf *bb, u8 byte)
{
    bb_grow(bb, 1);
    bb->data[bb->len++] = byte;
}

static void bb_append_u32(ByteBuf *bb, u32 val)
{
    bb_grow(bb, 4);
    bb->data[bb->len++] = (u8) (val & 0xFF);
    bb->data[bb->len++] = (u8) ((val >> 8) & 0xFF);
    bb->data[bb->len++] = (u8) ((val >> 16) & 0xFF);
    bb->data[bb->len++] = (u8) ((val >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Machine-code emission                                               */
/* ------------------------------------------------------------------ */

static void emit_instr_mc(Instr *in, ByteBuf *bb)
{
    switch (in->opcode)
    {
        case OP_RET:
            if (in->nops > 0 && in->ops[0].is_imm)
            {
                i64 imm = in->ops[0].u.imm;
                ASSERT(imm >= INT32_MIN && imm <= INT32_MAX);
                bb_append(bb, 0xB8); /* movl $imm32, %eax */
                bb_append_u32(bb, (u32) (i32) imm);
            }
            bb_append(bb, 0xC3); /* ret */
            break;
        case OP_UNREACHABLE:
            bb_append(bb, 0x0F); /* ud2 */
            bb_append(bb, 0x0B);
            break;
        default:
            /* Unsupported opcode: trap at runtime. */
            bb_append(bb, 0x0F); /* ud2 */
            bb_append(bb, 0x0B);
            fprintf(stderr, "[codegen] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
            break;
    }
}

static void emit_func_mc(Function *f, CodegenFunc *cf, Arena *arena)
{
    ByteBuf bb;
    bb_init(&bb, arena);

    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            emit_instr_mc(in, &bb);
        }
    }

    cf->name = f->name;
    cf->bytes = bb.data;
    cf->len = bb.len;
    cf->cap = bb.cap;
}

CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena)
{
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->ir = ir;
    cm->funcs = vec_new(arena);

    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        Function *f = (Function *) vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        emit_func_mc(f, cf, arena);
        vec_push(cm->funcs, cf);
    }

    return cm;
}

/* ------------------------------------------------------------------ */
/* Text assembly emission                                              */
/* ------------------------------------------------------------------ */

static void emit_instr_text(Instr *in, Sbuf *text)
{
    switch (in->opcode)
    {
        case OP_RET:
            if (in->nops > 0 && in->ops[0].is_imm)
            {
                i64 imm = in->ops[0].u.imm;
                ASSERT(imm >= INT32_MIN && imm <= INT32_MAX);
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) imm);
            }
            sbuf_append(text, "    ret\n");
            break;
        case OP_UNREACHABLE:
            sbuf_append(text, "    ud2\n");
            break;
        default:
            sbuf_appendf(text, "    # unsupported opcode %s\n", ir_opcode_name(in->opcode));
            sbuf_append(text, "    ud2\n");
            fprintf(stderr, "[codegen] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
            break;
    }
}

static void emit_func_text(Function *f, Sbuf *text)
{
    sbuf_appendf(text, ".globl %s\n", f->name);
    sbuf_appendf(text, ".type %s, @function\n", f->name);
    sbuf_appendf(text, "%s:\n", f->name);

    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        /* Emit label for non-entry blocks */
        if (bi > 0)
            sbuf_appendf(text, "%s:\n", blk->label);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            emit_instr_text(in, text);
        }
    }

    sbuf_appendf(text, ".size %s, .-%s\n", f->name, f->name);
}

void codegen_text_dump(CodegenModule *cm, Sbuf *out)
{
    sbuf_append(out, ".text\n");
    size_t nfuncs = vec_size(cm->ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        Function *f = (Function *) vec_get(cm->ir->funcs, i);
        emit_func_text(f, out);
    }
}
