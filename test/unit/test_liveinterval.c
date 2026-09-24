#include "harness.h"
#include "ir.h"
#include "liveinterval.h"
#include "regalloc.h"
#include "spill.h"
#include "util/arena.h"
#include "util/vec.h"

static const LiveInterval *find_iv(const LiveIntervals *set, u32 vreg)
{
    for (u32 i = 0; i < set->n; i++)
    {
        if (set->ivs[i].vreg == vreg)
        {
            return &set->ivs[i];
        }
    }
    return NULL;
}

static void add_param(IrFunction *f, Arena *a, Type *t, u32 vreg, const char *name)
{
    IrParam *p = arena_alloc(a, sizeof(IrParam), sizeof(void *));
    p->name = name;
    p->type = t;
    p->vreg = vreg;
    vec_push(f->params, p);
}

static IrModule *build_linear(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 x = ir_alloc_vreg(m, 8, true, false);
    u32 y = ir_alloc_vreg(m, 8, true, false);
    u32 z = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, x, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, y, ir_operand_vreg(x), ir_operand_imm(3));
    ir_emit_binop(entry, OP_ADD, z, ir_operand_vreg(y), ir_operand_imm(4));
    ir_emit_ret(entry, ir_operand_vreg(z));
    return m;
}

/* if/else diamond whose merge consumes a two-entry phi. */
static IrModule *build_merge(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *then_bb = ir_func_add_block(f, "then");
    IrBlock *else_bb = ir_func_add_block(f, "else");
    IrBlock *merge = ir_func_add_block(f, "merge");
    u32 cond = ir_alloc_vreg(m, 8, false, false);
    u32 val = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ICMP_SLT, cond, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_brcond(entry, ir_operand_vreg(cond), then_bb->label, else_bb->label);
    vec_push(then_bb->preds, entry);
    vec_push(else_bb->preds, entry);
    ir_emit_br(then_bb, merge->label);
    ir_emit_br(else_bb, merge->label);
    vec_push(merge->preds, then_bb);
    vec_push(merge->preds, else_bb);
    IrInstr *phi = ir_emit_phi_at_start(merge, val, 2);
    ir_phi_add_entry(phi, ir_operand_imm(10), then_bb);
    ir_phi_add_entry(phi, ir_operand_imm(20), else_bb);
    ir_emit_ret(merge, ir_operand_vreg(val));
    return m;
}

static IrModule *build_calls(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 r1 = ir_alloc_vreg(m, 8, true, false);
    u32 r2 = ir_alloc_vreg(m, 8, true, false);
    IrOperand *a1 = arena_alloc(a, sizeof(IrOperand), sizeof(IrOperand));
    a1[0] = ir_operand_imm(1);
    ir_emit_call(entry, r1, "foo", 1, a1);
    ir_emit_call(entry, r2, "bar", 1, a1);
    ir_emit_ret(entry, ir_operand_vreg(r1));
    return m;
}

TEST(liveinterval, positions_number_instructions_with_gaps)
{
    Arena *a = arena_new();
    IrModule *m = build_linear(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    EXPECT_EQ(set.pos.nblocks, 1);
    EXPECT_EQ(set.pos.block_base[0], 0);
    EXPECT_EQ(set.pos.block_end[0], 8);
    EXPECT_EQ(set.pos.npositions, 8);
    arena_free(a);
}

TEST(liveinterval, single_block_live_ranges_are_exact)
{
    Arena *a = arena_new();
    IrModule *m = build_linear(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    EXPECT_EQ(find_iv(&set, 0)->start, 0);
    EXPECT_EQ(find_iv(&set, 0)->end, 2);
    EXPECT_EQ(find_iv(&set, 1)->start, 2);
    EXPECT_EQ(find_iv(&set, 1)->end, 4);
    /* z is read by ret, which runs after the pred-end copies of the block. */
    EXPECT_EQ(find_iv(&set, 2)->start, 4);
    EXPECT_EQ(find_iv(&set, 2)->end, 7);
    EXPECT_EQ(set.n, 3);
    arena_free(a);
}

TEST(liveinterval, phi_result_keeps_pred_ends)
{
    Arena *a = arena_new();
    IrModule *m = build_merge(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    /* val's copies run at the then-end (5) and else-end (7) gaps; its ret read
       in merge sits at position 11, past the copy zone. */
    EXPECT_EQ(find_iv(&set, 1)->start, 5);
    EXPECT_EQ(find_iv(&set, 1)->end, 11);
    arena_free(a);
}

TEST(liveinterval, dead_def_still_occupies_its_position)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 live = ir_alloc_vreg(m, 8, true, false);
    u32 dead = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, live, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, dead, ir_operand_imm(3), ir_operand_imm(4));
    ir_emit_ret(entry, ir_operand_vreg(live));
    LiveIntervals set = liveinterval_compute(f, m, a);
    EXPECT_NOTNULL(find_iv(&set, dead));
    EXPECT_EQ(find_iv(&set, dead)->start, 2);
    EXPECT_EQ(find_iv(&set, dead)->end, 2);
    arena_free(a);
}

TEST(liveinterval, width_and_class_inference)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 d = ir_alloc_fp_vreg(m, 8);
    u32 flt = ir_alloc_fp_vreg(m, 4);
    u32 ld = ir_alloc_fp_vreg(m, 16);
    u32 i = ir_alloc_vreg(m, 8, true, false);
    add_param(f, a, type_double(), d, "d");
    add_param(f, a, type_float(), flt, "f");
    add_param(f, a, type_long_double(), ld, "ld");
    add_param(f, a, type_int(), i, "i");
    ir_func_add_block(f, "entry");
    LiveIntervals set = liveinterval_compute(f, m, a);
    EXPECT_EQ(find_iv(&set, d)->cls, RC_XMM);
    EXPECT_EQ(find_iv(&set, flt)->cls, RC_XMM);
    EXPECT_EQ(find_iv(&set, ld)->cls, RC_X87);
    EXPECT_EQ(find_iv(&set, i)->cls, RC_GPR);
    EXPECT_EQ(find_iv(&set, ld)->width, 16);
    arena_free(a);
}

TEST(liveinterval, overlap_never_shared_despite_first_fit)
{
    Arena *a = arena_new();
    IrModule *m = build_linear(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    RegAllocation *alloc = regalloc_all_spilled((IrFunction *) vec_get(m->funcs, 0), &set, a);
    for (u32 i = 0; i < set.n; i++)
    {
        for (u32 j = i + 1; j < set.n; j++)
        {
            const LiveInterval *u = &set.ivs[i];
            const LiveInterval *v = &set.ivs[j];
            bool overlap = u->start <= v->end && v->start <= u->end;
            if (overlap)
            {
                EXPECT_TRUE(alloc->slot_map[u->vreg] != alloc->slot_map[v->vreg]);
            }
        }
    }
    arena_free(a);
}

TEST(liveinterval, disjoint_ranges_reuse_the_slot)
{
    Arena *a = arena_new();
    IrModule *m = build_linear(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    RegAllocation *alloc = regalloc_all_spilled((IrFunction *) vec_get(m->funcs, 0), &set, a);
    EXPECT_EQ(alloc->slot_map[0], 8);
    EXPECT_EQ(alloc->slot_map[1], 16);
    EXPECT_EQ(alloc->slot_map[2], 8);
    EXPECT_EQ(alloc->frame_size, 16);
    arena_free(a);
}

TEST(liveinterval, loc_at_resolves_vregs_and_immediates)
{
    Arena *a = arena_new();
    IrModule *m = build_linear(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    RegAllocation *alloc = regalloc_all_spilled((IrFunction *) vec_get(m->funcs, 0), &set, a);
    RegLoc lx = loc_at(alloc, ir_operand_vreg(0), 0);
    EXPECT_EQ(lx.kind, LOC_MEM);
    EXPECT_EQ(lx.disp, -8);
    RegLoc li = loc_at(alloc, ir_operand_imm(5), 0);
    EXPECT_EQ(li.kind, LOC_IMM);
    arena_free(a);
}

TEST(liveinterval, call_sites_recorded_at_their_positions)
{
    Arena *a = arena_new();
    IrModule *m = build_calls(a);
    LiveIntervals set = liveinterval_compute((IrFunction *) vec_get(m->funcs, 0), m, a);
    RegAllocation *alloc = regalloc_all_spilled((IrFunction *) vec_get(m->funcs, 0), &set, a);
    EXPECT_EQ(vec_size(alloc->call_sites), 2);
    EXPECT_EQ(*((u32 *) vec_get(alloc->call_sites, 0)), 0);
    EXPECT_EQ(*((u32 *) vec_get(alloc->call_sites, 1)), 2);
    arena_free(a);
}

TEST(liveinterval, computation_is_deterministic)
{
    Arena *a1 = arena_new();
    Arena *a2 = arena_new();
    IrModule *m1 = build_merge(a1);
    IrModule *m2 = build_merge(a2);
    LiveIntervals s1 = liveinterval_compute((IrFunction *) vec_get(m1->funcs, 0), m1, a1);
    LiveIntervals s2 = liveinterval_compute((IrFunction *) vec_get(m2->funcs, 0), m2, a2);
    EXPECT_EQ(s1.n, s2.n);
    for (u32 i = 0; i < s1.n; i++)
    {
        EXPECT_EQ(s1.ivs[i].vreg, s2.ivs[i].vreg);
        EXPECT_EQ(s1.ivs[i].start, s2.ivs[i].start);
        EXPECT_EQ(s1.ivs[i].end, s2.ivs[i].end);
    }
    arena_free(a1);
    arena_free(a2);
}