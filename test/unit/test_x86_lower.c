#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/bytebuf.h"
#include "x86_lower.h"

static RegAllocation gap_alloc(Arena *arena, u32 vreg, u8 pre_reg, u8 post_reg)
{
    RegAllocation alloc = {0};
    alloc.nvregs = vreg + 1;
    alloc.segments = arena_alloc(arena, 2 * sizeof(RegSegment), _Alignof(RegSegment));
    alloc.segments[0] = (RegSegment) {0, 1, SEG_REG, pre_reg, 0};
    alloc.segments[1] = (RegSegment) {2, 3, SEG_REG, post_reg, 0};
    alloc.nsegments = 2;
    alloc.seg_begin = arena_alloc(arena, (vreg + 2) * sizeof(u32), sizeof(u32));
    for (u32 i = 0; i < vreg + 2; i++)
    {
        alloc.seg_begin[i] = 0;
    }
    alloc.seg_begin[vreg + 1] = 2;
    CallGap *gap = arena_alloc(arena, sizeof(CallGap), _Alignof(CallGap));
    gap->pos = 2;
    gap->vreg = vreg;
    alloc.call_gaps = gap;
    alloc.ncall_gaps = 1;
    alloc.slot_map = arena_alloc(arena, (vreg + 1) * sizeof(u32), sizeof(u32));
    alloc.slot_map[vreg] = 16;
    return alloc;
}

static u32 gap_vreg(IrModule *m)
{
    ir_module_add_func(m, "main", type_int());
    return ir_alloc_vreg(m, 8, true, false);
}

TEST(x86_lower, same_callee_saved_call_gap_emits_no_moves)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    u32 v = gap_vreg(m);
    RegAllocation alloc = gap_alloc(a, v, R_EBX, R_EBX);
    ByteBuf b;
    bytebuf_init(&b, a);
    LinearFrame frame = {0};
    X86LowerCtx ctx = {.mod = m,
                       .buf = &b,
                       .alloc = &alloc,
                       .frame = &frame,
                       .target = x86_64_target(),
                       .cur_pos = 2};
    x86_lower_call_gaps(&ctx, true);
    EXPECT_EQ(bytebuf_len(&b), 0u);
    x86_lower_call_gaps(&ctx, false);
    EXPECT_EQ(bytebuf_len(&b), 0u);
    arena_free(a);
}

TEST(x86_lower, distinct_callee_saved_call_gap_coalesces_to_one_move)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    u32 v = gap_vreg(m);
    RegAllocation alloc = gap_alloc(a, v, R_EBX, R_R12);
    ByteBuf b;
    bytebuf_init(&b, a);
    LinearFrame frame = {0};
    X86LowerCtx ctx = {.mod = m,
                       .buf = &b,
                       .alloc = &alloc,
                       .frame = &frame,
                       .target = x86_64_target(),
                       .cur_pos = 2};
    x86_lower_call_gaps(&ctx, true);
    EXPECT_EQ(bytebuf_len(&b), 0u);
    x86_lower_call_gaps(&ctx, false);
    EXPECT_EQ(bytebuf_len(&b), 3u); /* mov %r12, %rbx */
    arena_free(a);
}

TEST(x86_lower, caller_saved_call_gap_stores_before_and_reloads_after)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    u32 v = gap_vreg(m);
    RegAllocation alloc = gap_alloc(a, v, R_EAX, R_ECX);
    ByteBuf b;
    bytebuf_init(&b, a);
    LinearFrame frame = {0};
    X86LowerCtx ctx = {.mod = m,
                       .buf = &b,
                       .alloc = &alloc,
                       .frame = &frame,
                       .target = x86_64_target(),
                       .cur_pos = 2};
    x86_lower_call_gaps(&ctx, true);
    size_t stored = bytebuf_len(&b);
    EXPECT_TRUE(stored > 0);
    x86_lower_call_gaps(&ctx, false);
    EXPECT_TRUE(bytebuf_len(&b) > stored);
    arena_free(a);
}

/* A 32-bit `mov r,r` (no REX.W) is the redundant self-move a zext emits to
   clear the upper half. */
static bool has_32bit_self_move(ByteBuf *b)
{
    const u8 *code = bytebuf_data(b);
    size_t n = bytebuf_len(b);
    for (size_t i = 0; i + 1 < n; i++)
    {
        if (code[i] != 0x89 || (i > 0 && code[i - 1] >= 0x48 && code[i - 1] <= 0x4F))
        {
            continue;
        }
        u8 modrm = code[i + 1];
        if ((modrm & 0xC0) == 0xC0 && ((modrm >> 3) & 7) == (modrm & 7))
        {
            return true;
        }
    }
    return false;
}

TEST(x86_lower, zero_extended_source_skips_the_zext_self_move)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("unsigned long w(unsigned x) { return x + 1u; }\n", a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    ByteBuf *bytes = ((CodegenFunc *) vec_get(cm->funcs, 0))->bytes;
    EXPECT_FALSE(has_32bit_self_move(bytes));
    arena_free(a);
}

/* IMUL r,r,imm (imm8 `6B` or imm32 `69`) with a register ModRM. */
static bool has_imul_imm(ByteBuf *b)
{
    const u8 *code = bytebuf_data(b);
    size_t n = bytebuf_len(b);
    for (size_t i = 0; i + 1 < n; i++)
    {
        if ((code[i] == 0x69 || code[i] == 0x6B) && (code[i + 1] & 0xC0) == 0xC0)
        {
            return true;
        }
    }
    return false;
}

TEST(x86_lower, power_of_two_gep_stride_uses_a_shift)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("struct S16 { long a; long b; };\n"
                                  "long get(struct S16 *p, int i) { return p[i].a; }\n",
                                  a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    ByteBuf *bytes = ((CodegenFunc *) vec_get(cm->funcs, 0))->bytes;
    EXPECT_FALSE(has_imul_imm(bytes));
    arena_free(a);
}

/* A rel32 `jmp` whose displacement is zero targets the next instruction. */
static size_t count_jmps_to_next(ByteBuf *b)
{
    const u8 *code = bytebuf_data(b);
    size_t n = 0;
    for (size_t i = 0; i + 5 <= bytebuf_len(b); i++)
    {
        if (code[i] == 0xE9 && code[i + 1] == 0 && code[i + 2] == 0 && code[i + 3] == 0 &&
            code[i + 4] == 0)
        {
            n++;
        }
    }
    return n;
}

TEST(x86_lower, shared_epilogue_falls_through_from_the_final_return)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int pick(int x) { if (x) return 1; return 0; }\n", a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    ByteBuf *bytes = ((CodegenFunc *) vec_get(cm->funcs, 0))->bytes;
    EXPECT_EQ(count_jmps_to_next(bytes), 0u);
    arena_free(a);
}

/* REX.W `sub rsp, imm` (`81 EC` imm32 or `83 EC` imm8). */
static bool has_rsp_sub(ByteBuf *b)
{
    const u8 *code = bytebuf_data(b);
    for (size_t i = 0; i + 3 < bytebuf_len(b); i++)
    {
        if (code[i] == 0x48 && (code[i + 1] == 0x81 || code[i + 1] == 0x83) && code[i + 2] == 0xEC)
        {
            return true;
        }
    }
    return false;
}

TEST(x86_lower, phi_copy_cycle_alone_reserves_the_scratch_slot)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(
        "int swap(int n) { int a = 1; int b = 2; while (n > 0) { int t = a; a = b; b = t; "
        "n = n - 1; } return a - b; }\n"
        "int pick(int c) { int x; if (c) x = 1; else x = 2; return x; }\n",
        a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    ByteBuf *swap = ((CodegenFunc *) vec_get(cm->funcs, 0))->bytes;
    ByteBuf *pick = ((CodegenFunc *) vec_get(cm->funcs, 1))->bytes;
    EXPECT_TRUE(has_rsp_sub(swap));
    EXPECT_FALSE(has_rsp_sub(pick));
    arena_free(a);
}

static bool has_fp_prologue(ByteBuf *b)
{
    const u8 *code = bytebuf_data(b);
    for (size_t i = 0; i + 4 <= bytebuf_len(b); i++)
    {
        if (code[i] == 0x55 && code[i + 1] == 0x48 && code[i + 2] == 0x89 && code[i + 3] == 0xE5)
        {
            return true;
        }
    }
    return false;
}

TEST(x86_lower, spill_free_functions_omit_the_frame_pointer)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(
        "int pick(int c) { int x; if (c) x = 1; else x = 2; return x; }\n"
        "int swap(int n) { int a = 1; int b = 2; while (n > 0) { int t = a; a = b; b = t; "
        "n = n - 1; } return a - b; }\n"
        "int arr(int n) { char buf[16]; buf[0] = (char)n; return buf[0]; }\n",
        a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    for (size_t i = 0; i < 3; i++)
    {
        EXPECT_FALSE(has_fp_prologue(((CodegenFunc *) vec_get(cm->funcs, i))->bytes));
    }
    arena_free(a);
}

TEST(x86_lower, stack_argument_call_keeps_the_frame_pointer)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module(
        "long g(long a, long b, long c, long d, long e, long f, long h) { return a + h; }\n"
        "long call7(long x) { return g(x, 1, 2, 3, 4, 5, 6); }\n",
        a);
    EXPECT_NOTNULL(m);
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    ByteBuf *bytes = ((CodegenFunc *) vec_get(cm->funcs, 1))->bytes;
    EXPECT_TRUE(has_fp_prologue(bytes));
    arena_free(a);
}

static u32 zero_offset_gep_result(IrFunction *f)
{
    for (size_t b = 0; b < vec_size(f->blocks); b++)
    {
        IrBlock *blk = (IrBlock *) vec_get(f->blocks, b);
        for (size_t ii = 0; ii < vec_size(blk->instrs); ii++)
        {
            IrInstr *in = (IrInstr *) vec_get(blk->instrs, ii);
            if (in->opcode == OP_GEP && in->ops[1].is_imm && in->ops[2].is_imm &&
                in->ops[1].u.imm * in->ops[2].u.imm == 0)
            {
                return in->result;
            }
        }
    }
    return NO_VREG;
}

TEST(x86_lower, identity_geps_fold_before_liveness)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("struct S { int a; int b; };\n"
                                  "int first(struct S *s) { return s->a; }\n",
                                  a);
    EXPECT_NOTNULL(m);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    u32 g = zero_offset_gep_result(f);
    EXPECT_TRUE(g != NO_VREG);
    /* The segment table backs DWARF locations and is retained only under -g. */
    CodegenConfig dbg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &dbg, a);
    EXPECT_NOTNULL(cm);
    EXPECT_EQ(zero_offset_gep_result(f), NO_VREG);
    const RegAllocation *alloc = ((CodegenFunc *) vec_get(cm->funcs, 0))->alloc;
    EXPECT_EQ(alloc->seg_begin[g], alloc->seg_begin[g + 1]);
    arena_free(a);
}
