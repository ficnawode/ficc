#include "codegen.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* Diagnostics carry no file:line:col — the IR carries no source locations. */
static void codegen_error(const char *fmt, ...)
{
    fprintf(stderr, "[codegen] error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

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
        {
            bb->cap *= 2;
        }
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

static void bb_append_i32(ByteBuf *bb, i32 val)
{
    bb_append_u32(bb, (u32) val);
}

static void bb_append_i8(ByteBuf *bb, i8 val)
{
    bb_append(bb, (u8) val);
}

/* ------------------------------------------------------------------ */
/* Patch records for PC-relative calls                                 */
/* ------------------------------------------------------------------ */

typedef struct CallPatch CallPatch;
struct CallPatch
{
    size_t offset; /* offset of the E8 byte within the function */
    const char *target;
};

typedef struct BlockPatch BlockPatch;
struct BlockPatch
{
    size_t offset;      /* offset of the jump instruction */
    const char *target; /* target block label */
};

typedef struct PhiCopy PhiCopy;
struct PhiCopy
{
    Operand src;
    u32 dst_vreg;
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
    return -(i32) ((vreg + 1) * 8);
}

static bool fits_i8(i32 v)
{
    return v >= -128 && v <= 127;
}

static bool fits_i32(i64 v)
{
    return v >= (i64) INT32_MIN && v <= (i64) INT32_MAX;
}

/* movl %reg, disp(%rbp) */
static void emit_mov_reg_to_rbp(ByteBuf *bb, u8 reg, i32 disp)
{
    if (reg >= 8)
    {
        bb_append(bb, rex(false, false, false, true));
    }
    bb_append(bb, 0x89);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, reg, 5));
        bb_append_i8(bb, (i8) disp);
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
    {
        bb_append(bb, rex(false, false, false, true));
    }
    bb_append(bb, 0x8B);
    if (fits_i8(disp))
    {
        bb_append(bb, modrm(1, reg, 5));
        bb_append_i8(bb, (i8) disp);
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

/* movl $imm32, %ecx */
static void emit_mov_imm_to_ecx(ByteBuf *bb, i32 imm)
{
    bb_append(bb, 0xB9);
    bb_append_i32(bb, imm);
}

/* movl %eax, %reg */
static void emit_mov_eax_to_reg(ByteBuf *bb, u8 reg)
{
    if (reg >= 8)
    {
        bb_append(bb, rex(false, false, false, true));
    }
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
        bb_append_i8(bb, (i8) disp);
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
        bb_append_i8(bb, (i8) imm);
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
        bb_append_i8(bb, (i8) disp);
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
        bb_append_i8(bb, (i8) imm);
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
        bb_append_i8(bb, (i8) disp);
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
        bb_append_i8(bb, (i8) imm);
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

/* testl %eax, %eax */
static void emit_testl_eax_eax(ByteBuf *bb)
{
    bb_append(bb, 0x85);
    bb_append(bb, modrm(3, 0, 0));
}

/* jz rel32 (placeholder) */
static void emit_jz_placeholder(ByteBuf *bb, Vec *patches, const char *target, Arena *arena)
{
    BlockPatch *p = arena_alloc(arena, sizeof(BlockPatch), sizeof(void *));
    bb_append(bb, 0x0F);
    bb_append(bb, 0x84);
    p->offset = bb->len; /* offset of the displacement */
    p->target = target;
    vec_push(patches, p);
    bb_append_i32(bb, 0);
}

/* jmp rel32 (placeholder) */
static void emit_jmp_placeholder(ByteBuf *bb, Vec *patches, const char *target, Arena *arena)
{
    BlockPatch *p = arena_alloc(arena, sizeof(BlockPatch), sizeof(void *));
    bb_append(bb, 0xE9);
    p->offset = bb->len; /* offset of the displacement */
    p->target = target;
    vec_push(patches, p);
    bb_append_i32(bb, 0);
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
    bb_append_i32(bb, (i32) imm);
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
    bb_append_i32(bb, (i32) imm);
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
    Vec *patches;       /* CallPatch* */
    Vec *block_patches; /* BlockPatch* */
    Function *func;
    Arena *arena;
    Vec **phi_copies; /* per-block Vec<PhiCopy*>, indexed by block index */
};

static void emit_load_operand(ByteBuf *bb, Operand op)
{
    if (op.is_imm)
    {
        emit_mov_imm_to_eax(bb, (i32) op.u.imm);
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

/* Every immediate below is encoded in a signed 32-bit field. Values that do
   not fit (e.g. a C11 `long` constant, LP64) are user input the current
   32-bit-only lowering cannot represent: diagnose, never assert. */
static bool instr_has_bad_imm(Instr *in)
{
    for (u8 oi = 0; oi < in->nops; oi++)
    {
        if (in->ops[oi].is_imm && !fits_i32(in->ops[oi].u.imm))
        {
            return true;
        }
    }
    if (in->opcode == OP_CALL)
    {
        for (u32 a = 0; a < in->extra.call.nargs; a++)
        {
            if (in->extra.call.args[a].is_imm && !fits_i32(in->extra.call.args[a].u.imm))
            {
                return true;
            }
        }
    }
    return false;
}

static void emit_instr_mc(Instr *in, CodegenCtx *ctx)
{
    ByteBuf *bb = ctx->bb;
    if (instr_has_bad_imm(in))
    {
        codegen_error("%s: immediate outside i32 range; 64-bit codegen not implemented yet",
                      ir_opcode_name(in->opcode));
        bb_append(bb, 0x0F); /* ud2 */
        bb_append(bb, 0x0B);
        return;
    }
    switch (in->opcode)
    {
        case OP_ADD:
        {
            emit_load_operand(bb, in->ops[0]);
            if (in->ops[1].is_imm)
            {
                emit_addl_imm_to_eax(bb, (i32) in->ops[1].u.imm);
            }
            else
            {
                emit_addl_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
            }
            emit_store_eax(bb, in->result);
            break;
        }
        case OP_SUB:
        {
            emit_load_operand(bb, in->ops[0]);
            if (in->ops[1].is_imm)
            {
                emit_subl_imm_to_eax(bb, (i32) in->ops[1].u.imm);
            }
            else
            {
                emit_subl_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
            }
            emit_store_eax(bb, in->result);
            break;
        }
        case OP_MUL:
        {
            emit_load_operand(bb, in->ops[0]);
            if (in->ops[1].is_imm)
            {
                emit_imull_imm_to_eax(bb, (i32) in->ops[1].u.imm);
            }
            else
            {
                emit_imull_rbp_to_eax(bb, vreg_offset(in->ops[1].u.vreg));
            }
            emit_store_eax(bb, in->result);
            break;
        }
        case OP_SDIV:
        {
            emit_load_operand(bb, in->ops[0]);
            emit_cdq(bb);
            if (in->ops[1].is_imm)
            {
                /* Load the divisor into %ecx directly: %eax still holds the
                   dividend (edx:eax after cdq) and must not be clobbered. */
                emit_mov_imm_to_ecx(bb, (i32) in->ops[1].u.imm);
            }
            else
            {
                emit_mov_rbp_to_reg(bb, 1,
                                    vreg_offset(in->ops[1].u.vreg)); /* mov disp(%rbp), %ecx */
            }
            emit_idivl_ecx(bb);
            emit_store_eax(bb, in->result);
            break;
        }
        case OP_SREM:
        {
            emit_load_operand(bb, in->ops[0]);
            emit_cdq(bb);
            if (in->ops[1].is_imm)
            {
                emit_mov_imm_to_ecx(bb, (i32) in->ops[1].u.imm);
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
        case OP_NEG:
        {
            emit_load_operand(bb, in->ops[0]);
            emit_negl_eax(bb);
            emit_store_eax(bb, in->result);
            break;
        }
        case OP_CALL:
        {
            u32 nargs = in->extra.call.nargs;
            /* Args 7+ go on the stack. Compute padding for 16-byte alignment. */
            u32 n_stack = (nargs > 6) ? (nargs - 6) : 0;
            u32 pad = (n_stack % 2) * 8;
            u32 total_stack = n_stack * 8 + pad;

            if (total_stack > 0)
            {
                if (total_stack <= 127)
                {
                    emit_sub_rsp_imm8(bb, (i8) total_stack);
                }
                else
                {
                    emit_sub_rsp_imm32(bb, total_stack);
                }
            }

            /* Store stack args at 0(%rsp), 8(%rsp), ... */
            for (u32 i = 6; i < nargs; i++)
            {
                Operand arg = in->extra.call.args[i];
                if (arg.is_imm)
                {
                    emit_mov_imm_to_eax(bb, (i32) arg.u.imm);
                }
                else
                {
                    emit_mov_rbp_to_reg(bb, 0, vreg_offset(arg.u.vreg));
                }
                if (i == 6)
                {
                    emit_mov_eax_to_rsp(bb);
                }
                else
                {
                    emit_mov_eax_to_rsp_disp8(bb, (i8) ((i - 6) * 8));
                }
            }

            /* Load register args (reverse order to avoid clobbering) */
            static const u8 arg_regs[6] = {7, 6, 2, 1, 8, 9}; /* rdi, rsi, rdx, rcx, r8, r9 */
            for (i32 i = (i32) nargs - 1; i >= 0 && i < 6; i--)
            {
                Operand arg = in->extra.call.args[i];
                if (arg.is_imm)
                {
                    emit_mov_imm_to_eax(bb, (i32) arg.u.imm);
                }
                else
                {
                    emit_mov_rbp_to_reg(bb, 0, vreg_offset(arg.u.vreg));
                }
                emit_mov_eax_to_reg(bb, arg_regs[i]);
            }

            emit_call_placeholder(bb, ctx->patches, in->extra.call.name, ctx->arena);

            /* Store result */
            emit_store_eax(bb, in->result);

            if (total_stack > 0)
            {
                if (total_stack <= 127)
                {
                    emit_add_rsp_imm8(bb, (i8) total_stack);
                }
                else
                {
                    emit_add_rsp_imm32(bb, total_stack);
                }
            }
            break;
        }
        case OP_RET:
        {
            if (in->nops > 0)
            {
                emit_load_operand(bb, in->ops[0]);
            }
            else
            {
                emit_mov_imm_to_eax(bb, 0);
            }
            bb_append(bb, 0xC9); /* leave */
            bb_append(bb, 0xC3); /* ret */
            break;
        }
        case OP_BR:
        {
            emit_jmp_placeholder(bb, ctx->block_patches, in->extra.br.target_label, ctx->arena);
            break;
        }
        case OP_BRCOND:
        {
            emit_load_operand(bb, in->ops[0]);
            emit_testl_eax_eax(bb);
            emit_jz_placeholder(bb, ctx->block_patches, in->extra.brcond.false_label, ctx->arena);
            break;
        }
        case OP_PHI:
            /* No machine code: PHI nodes are lowered into copies in predecessor blocks */
            break;
        case OP_UNREACHABLE:
            bb_append(bb, 0x0F); /* ud2 */
            bb_append(bb, 0x0B);
            break;
        default:
            bb_append(bb, 0x0F); /* ud2 */
            bb_append(bb, 0x0B);
            codegen_error("unsupported opcode %s", ir_opcode_name(in->opcode));
            break;
    }
}

static u32 compute_max_vreg(Function *f)
{
    u32 max = 0;
    size_t nblocks = vec_size(f->blocks);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            if (in->result != NO_VREG && in->result > max)
            {
                max = in->result;
            }
            for (u8 oi = 0; oi < in->nops; oi++)
            {
                if (!in->ops[oi].is_imm && in->ops[oi].u.vreg > max)
                {
                    max = in->ops[oi].u.vreg;
                }
            }
            if (in->opcode == OP_CALL)
            {
                for (u32 a = 0; a < in->extra.call.nargs; a++)
                {
                    Operand arg = in->extra.call.args[a];
                    if (!arg.is_imm && arg.u.vreg > max)
                    {
                        max = arg.u.vreg;
                    }
                }
            }
        }
    }
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *) vec_get(f->params, i);
        if (p->vreg > max)
        {
            max = p->vreg;
        }
    }
    return max;
}

static void emit_func_mc(Function *f, CodegenFunc *cf, Arena *arena)
{
    ByteBuf bb;
    bb_init(&bb, arena);
    Vec *patches = vec_new(arena);
    Vec *block_patches = vec_new(arena);

    u32 max_vreg = compute_max_vreg(f);
    u32 frame_size = (u32) (((max_vreg + 1) * 8 + 15) & ~15ULL);

    /* Prologue */
    bb_append(&bb, 0x55); /* push %rbp */
    bb_append(&bb, 0x48); /* mov %rsp, %rbp */
    bb_append(&bb, 0x89);
    bb_append(&bb, 0xE5);
    if (frame_size > 0)
    {
        if (frame_size <= 127)
        {
            emit_sub_rsp_imm8(&bb, (i8) frame_size);
        }
        else
        {
            emit_sub_rsp_imm32(&bb, frame_size);
        }
    }

    /* Move incoming args into param vreg slots */
    static const u8 param_regs[6] = {7, 6, 2, 1, 8, 9};
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *) vec_get(f->params, i);
        i32 off = vreg_offset(p->vreg);
        if (i < 6)
        {
            emit_mov_reg_to_rbp(&bb, param_regs[i], off);
        }
        else
        {
            /* Stack arg at 16 + (i-6)*8(%rbp) */
            i32 arg_off = 16 + (i32) ((i - 6) * 8);
            emit_mov_rbp_to_reg(&bb, 0, arg_off);
            emit_mov_reg_to_rbp(&bb, 0, off);
        }
    }

    /* Build per-block phi-copies map */
    size_t nblocks = vec_size(f->blocks);
    Vec **phi_copies = arena_alloc(arena, nblocks * sizeof(Vec *), sizeof(void *));
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        phi_copies[bi] = vec_new(arena);
    }

    /* Label -> Block map for O(1) predecessor lookup */
    StrMap *label_to_block = strmap_new(arena);
    /* Block pointer -> index map for O(1) index lookup */
    U64Map *block_to_index = u64map_new(arena);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        strmap_set(label_to_block, blk->label, blk);
        u64map_set(block_to_index, (u64) (uintptr_t) blk, (void *) bi);
    }

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI)
            {
                continue;
            }
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                PhiEntry *entry = &in->extra.phi.entries[e];
                Block *pred = strmap_get(label_to_block, entry->label);
                ASSERT(pred != NULL && "phi entry names a real predecessor in this function");
                size_t pj = (size_t) u64map_get(block_to_index, (u64) (uintptr_t) pred);
                PhiCopy *pc = arena_alloc(arena, sizeof(PhiCopy), sizeof(void *));
                pc->src = entry->val;
                pc->dst_vreg = in->result;
                vec_push(phi_copies[pj], pc);
            }
        }
    }

    size_t *block_offsets = arena_alloc(arena, nblocks * sizeof(size_t), sizeof(size_t));

    /* Body */
    CodegenCtx ctx = {&bb, patches, block_patches, f, arena, phi_copies};
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        block_offsets[bi] = bb.len;
        size_t ninstr = vec_size(blk->instrs);

        /* Emit non-terminator instructions */
        size_t ii = 0;
        for (; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            if (in->opcode == OP_RET || in->opcode == OP_UNREACHABLE || in->opcode == OP_BR ||
                in->opcode == OP_BRCOND)
            {
                break;
            }
            emit_instr_mc(in, &ctx);
        }

        /* If there is a terminator, emit phi copies before it */
        if (ii < ninstr)
        {
            size_t npc = vec_size(phi_copies[bi]);
            for (size_t pi = 0; pi < npc; pi++)
            {
                PhiCopy *pc = (PhiCopy *) vec_get(phi_copies[bi], pi);
                emit_load_operand(&bb, pc->src);
                emit_store_eax(&bb, pc->dst_vreg);
            }
            emit_instr_mc((Instr *) vec_get(blk->instrs, ii), &ctx);
        }
    }

    /* Patch block-to-block jumps */
    size_t npatches = vec_size(block_patches);
    for (size_t pi = 0; pi < npatches; pi++)
    {
        BlockPatch *bp = (BlockPatch *) vec_get(block_patches, pi);
        /* Find target block offset */
        size_t target_off = 0;
        bool found = false;
        for (size_t bi = 0; bi < nblocks; bi++)
        {
            Block *blk = (Block *) vec_get(f->blocks, bi);
            if (strcmp(blk->label, bp->target) == 0)
            {
                target_off = block_offsets[bi];
                found = true;
                break;
            }
        }
        ASSERT(found && "branch target names a block the IR builder created");
        /* Compute relative offset from displacement end to target */
        size_t patch_end = bp->offset + 4; /* displacement is 4 bytes */
        i32 rel = (i32) ((i64) target_off - (i64) patch_end);
        bb.data[bp->offset + 0] = (u8) (rel & 0xFF);
        bb.data[bp->offset + 1] = (u8) ((rel >> 8) & 0xFF);
        bb.data[bp->offset + 2] = (u8) ((rel >> 16) & 0xFF);
        bb.data[bp->offset + 3] = (u8) ((rel >> 24) & 0xFF);
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
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        if (strcmp(cf->name, name) == 0)
        {
            return cf;
        }
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
        Function *f = (Function *) vec_get(ir->funcs, i);
        CodegenFunc *cf = arena_alloc(arena, sizeof(CodegenFunc), sizeof(void *));
        emit_func_mc(f, cf, arena);
        vec_push(cm->funcs, cf);
    }

    /* Compute function offsets in .text */
    size_t off = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        cf->offset = off;
        off += cf->len;
    }

    /* Apply call patches */
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t npatches = vec_size(cf->patches);
        for (size_t pi = 0; pi < npatches; pi++)
        {
            CallPatch *p = (CallPatch *) vec_get(cf->patches, pi);
            CodegenFunc *target = find_codegen_func(cm, p->target);
            if (!target)
            {
                codegen_error("undefined function '%s'", p->target);
                continue;
            }
            i32 rel = (i32) (target->offset - (cf->offset + p->offset + 5));
            /* Patch the 4 bytes after E8 */
            cf->bytes[p->offset + 1] = (u8) (rel & 0xFF);
            cf->bytes[p->offset + 2] = (u8) ((rel >> 8) & 0xFF);
            cf->bytes[p->offset + 3] = (u8) ((rel >> 16) & 0xFF);
            cf->bytes[p->offset + 4] = (u8) ((rel >> 24) & 0xFF);
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
        case OP_ADD:
        {
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n",
                         in->ops[0].is_imm ? 0 : vreg_offset(in->ops[0].u.vreg));
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            if (in->ops[1].is_imm)
            {
                sbuf_appendf(text, "    addl $%lld, %%eax\n", (long long) in->ops[1].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    addl %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
            }
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_SUB:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            if (in->ops[1].is_imm)
            {
                sbuf_appendf(text, "    subl $%lld, %%eax\n", (long long) in->ops[1].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    subl %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
            }
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_MUL:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            if (in->ops[1].is_imm)
            {
                sbuf_appendf(text, "    imull $%lld, %%eax\n", (long long) in->ops[1].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    imull %d(%%rbp), %%eax\n", vreg_offset(in->ops[1].u.vreg));
            }
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_SDIV:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            sbuf_appendf(text, "    cdq\n");
            if (in->ops[1].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%ecx\n", (long long) in->ops[1].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%ecx\n", vreg_offset(in->ops[1].u.vreg));
            }
            sbuf_appendf(text, "    idivl %%ecx\n");
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_SREM:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            sbuf_appendf(text, "    cdq\n");
            if (in->ops[1].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%ecx\n", (long long) in->ops[1].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%ecx\n", vreg_offset(in->ops[1].u.vreg));
            }
            sbuf_appendf(text, "    idivl %%ecx\n");
            sbuf_appendf(text, "    movl %%edx, %%eax\n");
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_NEG:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            sbuf_appendf(text, "    negl %%eax\n");
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(in->result));
            break;
        }
        case OP_CALL:
        {
            u32 nargs = in->extra.call.nargs;
            static const char *reg_names[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
            for (u32 i = 0; i < nargs && i < 6; i++)
            {
                if (in->extra.call.args[i].is_imm)
                {
                    sbuf_appendf(text, "    movl $%lld, %s\n",
                                 (long long) in->extra.call.args[i].u.imm, reg_names[i]);
                }
                else
                {
                    sbuf_appendf(text, "    movl %d(%%rbp), %s\n",
                                 vreg_offset(in->extra.call.args[i].u.vreg), reg_names[i]);
                }
            }
            if (nargs > 6)
            {
                u32 n_stack = nargs - 6;
                u32 pad = (n_stack % 2) * 8;
                if (pad)
                {
                    sbuf_appendf(text, "    subq $%u, %%rsp\n", pad);
                }
                for (i32 i = (i32) nargs - 1; i >= 6; i--)
                {
                    if (in->extra.call.args[i].is_imm)
                    {
                        sbuf_appendf(text, "    pushq $%lld\n",
                                     (long long) in->extra.call.args[i].u.imm);
                    }
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
        case OP_RET:
        {
            if (in->nops > 0 && in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else if (in->nops > 0)
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            sbuf_appendf(text, "    leave\n");
            sbuf_appendf(text, "    ret\n");
            break;
        }
        case OP_BR:
        {
            sbuf_appendf(text, "    jmp %s\n", in->extra.br.target_label);
            break;
        }
        case OP_BRCOND:
        {
            if (in->ops[0].is_imm)
            {
                sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) in->ops[0].u.imm);
            }
            else
            {
                sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", vreg_offset(in->ops[0].u.vreg));
            }
            sbuf_appendf(text, "    testl %%eax, %%eax\n");
            sbuf_appendf(text, "    jz %s\n", in->extra.brcond.false_label);
            break;
        }
        case OP_PHI:
        {
            sbuf_appendf(text, "    # phi v%u = ", in->result);
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                if (e > 0)
                {
                    sbuf_append(text, ", ");
                }
                if (in->extra.phi.entries[e].val.is_imm)
                {
                    sbuf_appendf(text, "imm %lld", (long long) in->extra.phi.entries[e].val.u.imm);
                }
                else
                {
                    sbuf_appendf(text, "v%u", in->extra.phi.entries[e].val.u.vreg);
                }
                sbuf_appendf(text, " from %s", in->extra.phi.entries[e].label);
            }
            sbuf_append(text, "\n");
            break;
        }
        case OP_UNREACHABLE:
            sbuf_appendf(text, "    ud2\n");
            break;
        default:
            sbuf_appendf(text, "    # unsupported opcode %s\n", ir_opcode_name(in->opcode));
            sbuf_appendf(text, "    ud2\n");
            codegen_error("unsupported opcode %s", ir_opcode_name(in->opcode));
            break;
    }
}

static void emit_func_text(Function *f, Sbuf *text, Arena *arena)
{
    sbuf_appendf(text, ".globl %s\n", f->name);
    sbuf_appendf(text, ".type %s, @function\n", f->name);
    sbuf_appendf(text, "%s:\n", f->name);

    /* Prologue */
    sbuf_appendf(text, "    pushq %%rbp\n");
    sbuf_appendf(text, "    movq %%rsp, %%rbp\n");
    u32 max_vreg = compute_max_vreg(f);
    u32 frame_size = (u32) (((max_vreg + 1) * 8 + 15) & ~15ULL);
    if (frame_size > 0)
    {
        sbuf_appendf(text, "    subq $%u, %%rsp\n", frame_size);
    }

    /* Param moves */
    static const char *param_regs[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    size_t nparams = vec_size(f->params);
    for (size_t i = 0; i < nparams; i++)
    {
        Param *p = (Param *) vec_get(f->params, i);
        if (i < 6)
        {
            sbuf_appendf(text, "    movl %s, %d(%%rbp)\n", param_regs[i], vreg_offset(p->vreg));
        }
        else
        {
            i32 arg_off = 16 + (i32) ((i - 6) * 8);
            sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n", arg_off);
            sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(p->vreg));
        }
    }

    /* Build per-block phi-copies map for text assembly */
    size_t nblocks = vec_size(f->blocks);
    Vec **phi_copies_text = arena_alloc(arena, nblocks * sizeof(Vec *), sizeof(void *));
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        phi_copies_text[bi] = vec_new(arena);
    }

    /* Reuse label -> block and block -> index maps */
    StrMap *label_to_block_text = strmap_new(arena);
    U64Map *block_to_index_text = u64map_new(arena);
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        strmap_set(label_to_block_text, blk->label, blk);
        u64map_set(block_to_index_text, (u64) (uintptr_t) blk, (void *) bi);
    }

    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            if (in->opcode != OP_PHI)
            {
                continue;
            }
            for (u32 e = 0; e < in->extra.phi.nentries; e++)
            {
                PhiEntry *entry = &in->extra.phi.entries[e];
                Block *pred = strmap_get(label_to_block_text, entry->label);
                ASSERT(pred != NULL && "phi entry names a real predecessor in this function");
                size_t pj = (size_t) u64map_get(block_to_index_text, (u64) (uintptr_t) pred);
                PhiCopy *pc = arena_alloc(arena, sizeof(PhiCopy), sizeof(void *));
                pc->src = entry->val;
                pc->dst_vreg = in->result;
                vec_push(phi_copies_text[pj], pc);
            }
        }
    }

    /* Body */
    for (size_t bi = 0; bi < nblocks; bi++)
    {
        Block *blk = (Block *) vec_get(f->blocks, bi);
        if (bi > 0)
        {
            sbuf_appendf(text, "%s:\n", blk->label);
        }
        size_t ninstr = vec_size(blk->instrs);
        for (size_t ii = 0; ii < ninstr; ii++)
        {
            Instr *in = (Instr *) vec_get(blk->instrs, ii);
            bool is_term = (in->opcode == OP_RET || in->opcode == OP_UNREACHABLE ||
                            in->opcode == OP_BR || in->opcode == OP_BRCOND);
            if (is_term)
            {
                size_t npc = vec_size(phi_copies_text[bi]);
                for (size_t pi = 0; pi < npc; pi++)
                {
                    PhiCopy *pc = (PhiCopy *) vec_get(phi_copies_text[bi], pi);
                    if (pc->src.is_imm)
                    {
                        sbuf_appendf(text, "    movl $%lld, %%eax\n", (long long) pc->src.u.imm);
                    }
                    else
                    {
                        sbuf_appendf(text, "    movl %d(%%rbp), %%eax\n",
                                     vreg_offset(pc->src.u.vreg));
                    }
                    sbuf_appendf(text, "    movl %%eax, %d(%%rbp)\n", vreg_offset(pc->dst_vreg));
                }
            }
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
        emit_func_text(f, out, cm->ir->arena);
    }
}
