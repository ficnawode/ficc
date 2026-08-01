#include "codegen.h"
#include "util/assert.h"
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Byte buffer helpers                                                 */
/* ------------------------------------------------------------------ */

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
    bb->data[bb->len++] = (u8)(val & 0xFF);
    bb->data[bb->len++] = (u8)((val >> 8) & 0xFF);
    bb->data[bb->len++] = (u8)((val >> 16) & 0xFF);
    bb->data[bb->len++] = (u8)((val >> 24) & 0xFF);
}

static void bb_append_i32(ByteBuf *bb, i32 val)
{
    bb_append_u32(bb, (u32)val);
}

static void bb_append_i8(ByteBuf *bb, i8 val)
{
    bb_append(bb, (u8)val);
}

/* ------------------------------------------------------------------ */
/* Patch records for PC-relative calls                                 */
/* ------------------------------------------------------------------ */

typedef struct CallPatch CallPatch;
struct CallPatch
{
    size_t offset;       /* offset of the E8 byte within the function */
    const char *target;
};

/* ------------------------------------------------------------------ */
/* x86-64 encoding helpers                                             */
/* ------------------------------------------------------------------ */

static u8 modrm(u8 mod, u8 reg, u8 rm)
{
    return (mod << 6) | ((reg & 7) << 3) | (rm & 7);
}

static u8 rex(bool w, bool r, bool x, bool b)
{
    return 0x40 | (w ? 0x08 : 0) | (r ? 0x04 : 0) | (x ? 0x02 : 0) | (b ? 0x01 : 0);
}

static i32 vreg_offset(u32 vreg)
{
    return -(i32)((vreg + 1) * 8);
}

static bool fits_i8(i32 v)
{
    return v >= -128 && v <= 127;
}

/* movl %reg, disp(%rbp) */
static void emit_mov_reg_to_rbp(ByteBuf *bb, u8 reg, i32 disp)
{
    if (reg >= 8)
        bb_append(bb, rex(false, false, false, true));
    bb_append(bb, 0x89);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, reg, 5));
        bb_append_i8(bb, (i8)disp);
    }
    else
    {
        bb_append(bb, modrm(2, reg, 5));
        bb_append_i32(bb, disp);
    }
}

/* movl disp(%rbp), %reg */
static void emit_mov_rbp_to_reg(ByteBuf *bb, u8 reg, i32 disp)
{
    if (reg >= 8)
        bb_append(bb, rex(false, false, false, true));
    bb_append(bb, 0x8B);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, reg, 5));
        bb_append_i8(bb, (i8)disp);
    }
    else
    {
        bb_append(bb, modrm(2, reg, 5));
        bb_append_i32(bb, disp);
    }
}

/* movl $imm32, %eax */
static void emit_mov_imm_to_eax(ByteBuf *bb, i32 imm)
{
    bb_append(bb, 0xB8);
    bb_append_i32(bb, imm);
}

/* movl %eax, %reg */
static void emit_mov_eax_to_reg(ByteBuf *bb, u8 reg)
{
    if (reg >= 8)
        bb_append(bb, rex(false, false, false, true));
    bb_append(bb, 0x89);
    bb_append(bb, modrm(3, 0, reg));
}

/* addl disp(%rbp), %eax */
static void emit_addl_rbp_to_eax(ByteBuf *bb, i32 disp)
{
    bb_append(bb, 0x03);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, 0, 5));
        bb_append_i8(bb, (i8)disp);
    }
    else
    {
        bb_append(bb, modrm(2, 0, 5));
        bb_append_i32(bb, disp);
    }
}

/* addl $imm, %eax */
static void emit_addl_imm_to_eax(ByteBuf *bb, i32 imm)
{
    if (fits_i8(imm))
    {
        bb_append(bb, 0x83);
        bb_append(bb, modrm(3, 0, 0));
        bb_append_i8(bb, (i8)imm);
    }
    else
    {
        bb_append(bb, 0x05);
        bb_append_i32(bb, imm);
    }
}

/* subl disp(%rbp), %eax */
static void emit_subl_rbp_to_eax(ByteBuf *bb, i32 disp)
{
    bb_append(bb, 0x2B);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, 0, 5));
        bb_append_i8(bb, (i8)disp);
    }
    else
    {
        bb_append(bb, modrm(2, 0, 5));
        bb_append_i32(bb, disp);
    }
}

/* subl $imm, %eax */
static void emit_subl_imm_to_eax(ByteBuf *bb, i32 imm)
{
    if (fits_i8(imm))
    {
        bb_append(bb, 0x83);
        bb_append(bb, modrm(3, 5, 0));
        bb_append_i8(bb, (i8)imm);
    }
    else
    {
        bb_append(bb, 0x2D);
        bb_append_i32(bb, imm);
    }
}

/* imull disp(%rbp), %eax */
static void emit_imull_rbp_to_eax(ByteBuf *bb, i32 disp)
{
    bb_append(bb, 0x0F);
    bb_append(bb, 0xAF);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, 0, 5));
        bb_append_i8(bb, (i8)disp);
    }
    else
    {
        bb_append(bb, modrm(2, 0, 5));
        bb_append_i32(bb, disp);
    }
}

/* imull $imm, %eax */
static void emit_imull_imm_to_eax(ByteBuf *bb, i32 imm)
{
    if (fits_i8(imm))
    {
        bb_append(bb, 0x6B);
        bb_append(bb, modrm(3, 0, 0));
        bb_append_i8(bb, (i8)imm);
    }
    else
    {
        bb_append(bb, 0x69);
        bb_append(bb, modrm(3, 0, 0));
        bb_append_i32(bb, imm);
    }
}

/* idivl %ecx */
static void emit_idivl_ecx(ByteBuf *bb)
{
    bb_append(bb, 0xF7);
    bb_append(bb, modrm(3, 7, 1));
}

/* negl %eax */
static void emit_negl_eax(ByteBuf *bb)
{
    bb_append(bb, 0xF7);
    bb_append(bb, modrm(3, 3, 0));
}

/* cdq */
static void emit_cdq(ByteBuf *bb)
{
    bb_append(bb, 0x99);
}

/* call rel32 (placeholder) */
static void emit_call_placeholder(ByteBuf *bb, Vec *patches, const char *target, Arena *arena)
{
    CallPatch *p = arena_alloc(arena, sizeof(CallPatch), sizeof(void *));
    p->offset = bb->len;
    p->target = target;
    vec_push(patches, p);
    bb_append(bb, 0xE8);
    bb_append_i32(bb, 0);
}

/* sub $imm, %rsp (64-bit, imm32) */
static void emit_sub_rsp_imm32(ByteBuf *bb, u32 imm)
{
    bb_append(bb, rex(true, false, false, false));
    bb_append(bb, 0x81);
    bb_append(bb, modrm(3, 5, 4));
    bb_append_i32(bb, (i32)imm);
}

/* sub $imm8, %rsp (64-bit, sign-extended) */
static void emit_sub_rsp_imm8(ByteBuf *bb, i8 imm)
{
    bb_append(bb, rex(true, false, false, false));
    bb_append(bb, 0x83);
    bb_append(bb, modrm(3, 5, 4));
    bb_append_i8(bb, imm);
}

/* add $imm, %rsp (64-bit, imm32) */
static void emit_add_rsp_imm32(ByteBuf *bb, u32 imm)
{
    bb_append(bb, rex(true, false, false, false));
    bb_append(bb, 0x81);
    bb_append(bb, modrm(3, 0, 4));
    bb_append_i32(bb, (i32)imm);
}

/* add $imm8, %rsp (64-bit) */
static void emit_add_rsp_imm8(ByteBuf *bb, i8 imm)
{
    bb_append(bb, rex(true, false, false, false));
    bb_append(bb, 0x83);
    bb_append(bb, modrm(3, 0, 4));
    bb_append_i8(bb, imm);
}

/* movl %eax, (%rsp) */
static void emit_mov_eax_to_rsp(ByteBuf *bb)
{
    bb_append(bb, 0x89);
    bb_append(bb, modrm(0, 0, 4));
    bb_append(bb, 0x24);
}

/* movl %eax, disp8(%rsp) */
static void emit_mov_eax_to_rsp_disp8(ByteBuf *bb, i8 disp)
{
    bb_append(bb, 0x89);
    bb_append(bb, modrm(1, 0, 4));
    bb_append(bb, 0x24);
    bb_append_i8(bb, disp);
}

/* ------------------------------------------------------------------ */
/* Machine-code emission                                               */
/* ------------------------------------------------------------------ */

typedef struct CodegenCtx CodegenCtx;
struct CodegenCtx
{
    ByteBuf *bb;
    Vec *patches;
    Function *func;
    Arena *arena;
};

static void emit_load_operand(ByteBuf *bb, Operand op, u32 nregs)
{
    (void)nregs;
    if (op.is_imm)
    {
        ASSERT(op.u.imm >= (i64)INT32_MIN && op.u.imm <= (i64)INT32_MAX);
        emit_mov_imm_to_eax(bb, (i32)op.u.imm);
    }
    else
    {
        emit_mov_rbp_to_reg(bb, 0, vreg_offset(op.u.vreg));
    }
}

static void emit_store_eax(ByteBuf *bb, u32 vreg)
{
    emit_mov_reg_to_rbp(bb, 0, vreg_offset(vreg));
}

static void emit_instr_mc(Instr *in, CodegenCtx *ctx)
{
    ByteBuf *bb = ctx->bb;
    switch (in->opcode)
    {
    case OP_ADD: {
        emit_load_operand(bb, in->ops[0], 0);
        if (in->ops[1].is_imm)
        {
            emit_addl_imm_to_eax(bb, (i32)in->ops[1].u.imm);
        }
        else
        {
            emit_addl_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
        }
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_SUB: {
        emit_load_operand(bb, in->ops[0], 0);
        if (in->ops[1].is_imm)
        {
            emit_subl_imm_to_eax(bb, (i32)in->ops[1].u.imm);
        }
        else
        {
            emit_subl_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
        }
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_MUL: {
        emit_load_operand(bb, in->ops[0], 0);
        if (in->ops[1].is_imm)
        {
            emit_imull_imm_to_eax(bb, (i32)in->ops[1].u.imm);
        }
        else
        {
            emit_imull_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
        }
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_SDIV: {
        emit_load_operand(bb, in->ops[0], 0);
        emit_cdq(bb);
        if (in->ops[1].is_imm)
        {
            emit_mov_imm_to_eax(bb, (i32)in->ops[1].u.imm);
            emit_mov_eax_to_reg(bb, 1); /* mov %eax, %ecx */
        }
        else
        {
            emit_mov_rbp_to_reg(bb, 1, vreg_offset(in->ops[1].u.vreg)); /* mov disp(%rbp), %ecx */
        }
        emit_idivl_ecx(bb);
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_SREM: {
        emit_load_operand(bb, in->ops[0], 0);
        emit_cdq(bb);
        if (in->ops[1].is_imm)
        {
            emit_mov_imm_to_eax(bb, (i32)in->ops[1].u.imm);
            emit_mov_eax_to_reg(bb, 1);
        }
        else
        {
            emit_mov_rbp_to_reg(bb, 1, vreg_offset(in->ops[1].u.vreg));
        }
        emit_idivl_ecx(bb);
        /* movl %edx, %eax */
        bb_append(bb, 0x89);
        bb_append(bb, modrm(3, 2, 0));
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_NEG: {
        emit_load_operand(bb, in->ops[0], 0);
        emit_negl_eax(bb);
        emit_store_eax(bb, in->result);
        break;
    }
    case OP_CALL: {
        u32 nargs = in->extra.call.nargs;
        /* Args 7+ go on the stack. Compute padding for 16-byte alignment. */
        u32 n_stack = (nargs > 6) ? (nargs - 6) : 0;
        u32 pad = (n_stack % 2) * 8;
        u32 total_stack = n_stack * 8 + pad;

        if (total_stack > 0)
        {
            if (total_stack <= 127)
                emit_sub_rsp_imm8(bb, (i8)total_stack);
            else
                emit_sub_rsp_imm32(bb, total_stack);
        }

        /* Store stack args at 0(%rsp), 8(%rsp), ... */
        for (u32 i = 6; i < nargs; i++)
        {
            Operand arg = in->extra.call.args[i];
            if (arg.is_imm)
                emit_mov_imm_to_eax(bb, (i32)arg.u.imm);
            else
                emit_mov_rbp_to_reg(bb, 0, vreg_offset(arg.u.vreg));
            if (i == 6)
                emit_mov_eax_to_rsp(bb);
            else
                emit_mov_eax_to_rsp_disp8(bb, (i8)((i - 6) * 8));
        }

        /* Load register args (reverse order to avoid clobbering) */
        static const u8 arg_regs[6] = {7, 6, 2, 1, 8, 9}; /* rdi, rsi, rdx, rcx, r8, r9 */
        for (i32 i = (i32)nargs - 1; i >= 0 && i < 6; i--)
        {
            Operand arg = in->extra.call.args[i];
            if (arg.is_imm)
                emit_mov_imm_to_eax(bb, (i32)arg.u.imm);
            else
                emit_mov_rbp_to_reg(bb, 0, vreg_offset(arg.u.vreg));
            emit_mov_eax_to_reg(bb, arg_regs[i]);
        }

        emit_call_placeholder(bb, ctx->patches, in->extra.call.name, ctx->arena);

        /* Store result */
        emit_store_eax(bb, in->result);

        if (total_stack > 0)
        {
            if (total_stack <= 127)
                emit_add_rsp_imm8(bb, (i8)total_stack);
            else
                emit_add_rsp_imm32(bb, total_stack);
        }
        break;
    }
    case OP_RET: {
        if (in->nops > 0)
        {
            emit_load_operand(bb, in->ops[0], 0);
        }
        else
        {
            emit_mov_imm_to_eax(bb, 0);
        }
        bb_append(bb, 0xC9); /* leave */
        bb_append(bb, 0xC3); /* ret */
        break;
    }
    case OP_UNREACHABLE:
        bb_append(bb, 0x0F); /* ud2 */
        bb_append(bb, 0x0B);
        break;
    default:
        bb_append(bb, 0x0F); /* ud2 */
        bb_append(bb, 0x0B);
        fprintf(stderr, "[codegen] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
        break;
    }
}

static u32 compute_max_vreg(Function *f)
{
    u32 max = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *)vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *)vec_get(blk->instrs, ii);
            if (in->result != NO_VREG && in->result > max)
                max = in->result;
            for (u8 oi = 0; oi < in->nops; oi++)
            {
                if (!in->ops[oi].is_imm && in->ops[oi].u.vreg > max)
                    max = in->ops[oi].u.vreg;
            }
            if (in->opcode == OP_CALL)
            {
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    Operand arg = in->extra.call.args[a];
                    if (!arg.is_imm && arg.u.vreg > max)
                        max = arg.u.vreg;
                }
            }
        }
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *)vec_get(f->params, i);
        if (p->vreg > max)
            max = p->vreg;
    }
    return max;
}

static void emit_func_mc(Function *f, CodegenFunc *cf, Arena *arena)
{
    ByteBuf bb;
    bb_init(&bb, arena);
    Vec *patches = vec_new(arena);

    u32 max_vreg = compute_max_vreg(f);
    u32 frame_size = (u32)(((max_vreg + 1) * 8 + 15) & ~15ULL);

    /* Prologue */
    bb_append(&bb, 0x55); /* push %rbp */
    bb_append(&bb, 0x48); /* mov %rsp, %rbp */
    bb_append(&bb, 0x89);
    bb_append(&bb, 0xE5);
    if (frame_size > 0)
    {
        if (frame_size <= 127)
            emit_sub_rsp_imm8(&bb, (i8)frame_size);
        else
            emit_sub_rsp_imm32(&bb, frame_size);
    }

    /* Move incoming args into param vreg slots */
    static const u8 param_regs[6] = {7, 6, 2, 1, 8, 9};
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *)vec_get(f->params, i);
        i32 off = vreg_offset(p->vreg);
        if (i < 6)
        {
            emit_mov_reg_to_rbp(&bb, param_regs[i], off);
        }
        else
        {
            /* Stack arg at 16 + (i-6)*8(%rbp) */
            i32 arg_off = 16 + (i32)((i - 6) * 8);
            emit_mov_rbp_to_reg(&bb, 0, arg_off);
            emit_mov_reg_to_rbp(&bb, 0, off);
        }
    }

    /* Body */
    CodegenCtx ctx = {&bb, patches, f, arena};
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *)vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *)vec_get(blk->instrs, ii);
            emit_instr_mc(in, &ctx);
        }
    }

    cf->name = f->name;
    cf->bytes = bb.data;
    cf->len = bb.len;
    cf->cap = bb.cap;
    cf->offset = 0;
    cf->patches = patches;
}

static CodegenFunc *find_codegen_func(CodegenModule *cm, const char *name)
{
    size_t n = vec_size(cm->funcs);
    for (size_t i = 0; i < n; i++)
    {
        CodegenFunc *cf = (CodegenFunc *)vec_get(cm->funcs, i);
        if (strcmp(cf->name, name) == 0)
            return cf;
    }
    return NULL;
}

CodegenModule *codegen_ir_to_machine(Module *ir, Arena *arena)
{
    CodegenModule *cm = arena_alloc(arena, sizeof(CodegenModule), sizeof(void *));
    cm->ir = ir;
    cm->funcs = vec_new(arena);

    size_t nfuncs = vec_size(ir->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        Function *f = (Function *)vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        emit_func_mc(f, cf, arena);
        vec_push(cm->funcs, cf);
    }

    /* Compute function offsets in .text */
    size_t off = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *)vec_get(cm->funcs, i);
        cf->offset = off;
        off += cf->len;
    }

    /* Apply call patches */
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *)vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            CallPatch *p = (CallPatch *)vec_get(cf->patches, pi);
            CodegenFunc *target = find_codegen_func(cm, p->target);
            if (!target)
            {
                fprintf(stderr, "[codegen] error: undefined function '%s'\n", p->target);
                continue;
            }
            i32 rel = (i32)(target->offset - (cf->offset + p->offset + 5));
            /* Patch the 4 bytes after E8 */
            cf->bytes[p->offset + 1] = (u8)(rel & 0xFF);
            cf->bytes[p->offset + 2] = (u8)((rel >> 8) & 0xFF);
            cf->bytes[p->offset + 3] = (u8)((rel >> 16) & 0xFF);
            cf->bytes[p->offset + 4] = (u8)((rel >> 24) & 0xFF);
        }
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
    case OP_ADD: {
        sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n",
                     in->ops[0].is_imm ? 0 : vreg_offset(in->ops[0].u.vreg));
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        if (in->ops[1].is_imm)
            sbuf_appendf(text, "    addl $%lld, %%eax\n", (long long)in->ops[1].u.imm);
        else
            sbuf_appendf(text, "    addl %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_SUB: {
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        if (in->ops[1].is_imm)
            sbuf_appendf(text, "    subl $%lld, %%eax\n", (long long)in->ops[1].u.imm);
        else
            sbuf_appendf(text, "    subl %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_MUL: {
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        if (in->ops[1].is_imm)
            sbuf_appendf(text, "    imull $%lld, %%eax\n", (long long)in->ops[1].u.imm);
        else
            sbuf_appendf(text, "    imull %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_SDIV: {
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        sbuf_appendf(text, "    cdq\n");
        if (in->ops[1].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%ecx\n", (long long)in->ops[1].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%ecx\n", vreg_offset(in->ops[1].u.vreg));
        sbuf_appendf(text, "    idivl %%ecx\n");
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_SREM: {
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        sbuf_appendf(text, "    cdq\n");
        if (in->ops[1].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%ecx\n", (long long)in->ops[1].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%ecx\n", vreg_offset(in->ops[1].u.vreg));
        sbuf_appendf(text, "    idivl %%ecx\n");
        sbuf_appendf(text, "    movl %%edx, %%eax\n");
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_NEG: {
        if (in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        sbuf_appendf(text, "    negl %%eax\n");
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_CALL: {
        u32 nargs = in->extra.call.nargs;
        static const char *reg_names[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
        for (u32 i = 0; i < nargs && i < 6; i++)
        {
            if (in->extra.call.args[i].is_imm)
                sbuf_appendf(text, "    movl $%lld, %s\n",
                             (long long)in->extra.call.args[i].u.imm, reg_names[i]);
            else
                sbuf_appendf(text, "    movl %d(%%rbp), %s\n",
                             vreg_offset(in->extra.call.args[i].u.vreg), reg_names[i]);
        }
        if (nargs > 6)
        {
            u32 n_stack = nargs - 6;
            u32 pad = (n_stack % 2) * 8;
            if (pad)
                sbuf_appendf(text, "    subq $%u, %%rsp\n", pad);
            for (i32 i = (i32)nargs - 1; i >= 6; i--)
            {
                if (in->extra.call.args[i].is_imm)
                    sbuf_appendf(text, "    pushq $%lld\n",
                                 (long long)in->extra.call.args[i].u.imm);
                else
                {
                    sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n",
                                 vreg_offset(in->extra.call.args[i].u.vreg));
                    sbuf_appendf(text, "    pushq %%rax\n");
                }
            }
        }
        sbuf_appendf(text, "    call %s\n", in->extra.call.name);
        if (nargs > 6)
        {
            u32 n_stack = nargs - 6;
            u32 pad = (n_stack % 2) * 8;
            u32 total = n_stack * 8 + pad;
            sbuf_appendf(text, "    addq $%u, %%rsp\n", total);
        }
        sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
        break;
    }
    case OP_RET: {
        if (in->nops > 0 && in->ops[0].is_imm)
            sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long)in->ops[0].u.imm);
        else if (in->nops > 0)
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
        sbuf_appendf(text, "    leave\n");
        sbuf_appendf(text, "    ret\n");
        break;
    }
    case OP_UNREACHABLE:
        sbuf_appendf(text, "    ud2\n");
        break;
    default:
        sbuf_appendf(text, "    # unsupported opcode %s\n", ir_opcode_name(in->opcode));
        sbuf_appendf(text, "    ud2\n");
        fprintf(stderr, "[codegen] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
        break;
    }
}

static void emit_func_text(Function *f, Sbuf *text)
{
    sbuf_appendf(text, ".globl %s\n", f->name);
    sbuf_appendf(text, ".type %s, @function\n", f->name);
    sbuf_appendf(text, "%s:\n", f->name);

    /* Prologue */
    sbuf_appendf(text, "    pushq %%rbp\n");
    sbuf_appendf(text, "    movq %%rsp, %%rbp\n");
    u32 max_vreg = compute_max_vreg(f);
    u32 frame_size = (u32)(((max_vreg + 1) * 8 + 15) & ~15ULL);
    if (frame_size > 0)
        sbuf_appendf(text, "    subq $%u, %%rsp\n", frame_size);

    /* Param moves */
    static const char *param_regs[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *)vec_get(f->params, i);
        if (i < 6)
        {
            sbuf_appendf(text, "    movl %s, %d(%%rbp)\n", param_regs[i], vreg_offset(p->vreg));
        }
        else
        {
            i32 arg_off = 16 + (i32)((i - 6) * 8);
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", arg_off);
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(p->vreg));
        }
    }

    /* Body */
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *)vec_get(f->blocks, bi);
        if (bi > 0)
            sbuf_appendf(text, "%s:\n", blk->label);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *)vec_get(blk->instrs, ii);
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
        Function *f = (Function *)vec_get(cm->ir->funcs, i);
        emit_func_text(f, out);
    }
}
