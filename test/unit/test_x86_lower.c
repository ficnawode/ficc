#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/bytebuf.h"
#include "x86_lower.h"

/* A two-segment split at the call position 2: [0,1] then [2,3]. */
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
    X86LowerCtx ctx = {
        .mod = m, .buf = &b, .alloc = &alloc, .target = x86_64_target(), .cur_pos = 2};
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
    X86LowerCtx ctx = {
        .mod = m, .buf = &b, .alloc = &alloc, .target = x86_64_target(), .cur_pos = 2};
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
    X86LowerCtx ctx = {
        .mod = m, .buf = &b, .alloc = &alloc, .target = x86_64_target(), .cur_pos = 2};
    x86_lower_call_gaps(&ctx, true);
    size_t stored = bytebuf_len(&b);
    EXPECT_TRUE(stored > 0);
    x86_lower_call_gaps(&ctx, false);
    EXPECT_TRUE(bytebuf_len(&b) > stored);
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
    CodegenModule *cm = codegen_ir_to_machine(m, NULL, a);
    EXPECT_NOTNULL(cm);
    EXPECT_EQ(zero_offset_gep_result(f), NO_VREG);
    const RegAllocation *alloc = ((CodegenFunc *) vec_get(cm->funcs, 0))->alloc;
    EXPECT_EQ(alloc->seg_begin[g], alloc->seg_begin[g + 1]);
    arena_free(a);
}
