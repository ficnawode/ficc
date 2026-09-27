#include "harness.h"
#include "ir.h"
#include "liveinterval.h"
#include "regalloc.h"
#include "target.h"
#include "type.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/vec.h"
#include "x86_emit.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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

static u32 emit_leaf(IrModule *m, IrBlock *b, i64 value)
{
    u32 v = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(b, OP_ADD, v, ir_operand_imm(value), ir_operand_imm(1));
    return v;
}

static bool is_callee_saved(u8 reg)
{
    return reg == R_EBX || reg == R_R12 || reg == R_R13 || reg == R_R14 || reg == R_R15;
}

static bool is_reserved(u8 reg)
{
    return reg == R_EAX || reg == R_ESP || reg == R_EBP || reg == R_R11;
}

static bool is_arg_lane(u8 reg)
{
    return reg == R_EDI || reg == R_ESI || reg == R_EDX || reg == R_ECX || reg == R_R8 ||
           reg == R_R9;
}

static IrModule *build_chain(Arena *a, u32 *x, u32 *y, u32 *z)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    *x = ir_alloc_vreg(m, 8, true, false);
    *y = ir_alloc_vreg(m, 8, true, false);
    *z = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *x, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, *y, ir_operand_vreg(*x), ir_operand_imm(3));
    ir_emit_binop(entry, OP_ADD, *z, ir_operand_vreg(*y), ir_operand_imm(4));
    ir_emit_ret(entry, ir_operand_vreg(*z));
    return m;
}

static IrModule *build_crossing_call(Arena *a, u32 *live, u32 *call_result)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    *live = ir_alloc_vreg(m, 8, true, false);
    *call_result = ir_alloc_vreg(m, 8, true, false);
    u32 sum = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *live, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_call(entry, *call_result, "foo", 0, NULL);
    ir_emit_binop(entry, OP_ADD, sum, ir_operand_vreg(*live), ir_operand_vreg(*call_result));
    ir_emit_ret(entry, ir_operand_vreg(sum));
    return m;
}

static IrModule *build_pressure(Arena *a)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 leaf[16];
    for (u32 i = 0; i < 16; i++)
    {
        leaf[i] = emit_leaf(m, entry, (i64) i + 1);
    }
    u32 acc = leaf[0];
    for (u32 i = 1; i < 16; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(leaf[i]));
        acc = next;
    }
    ir_emit_ret(entry, ir_operand_vreg(acc));
    return m;
}

static IrModule *build_phi_chain(Arena *a, u32 *v, u32 *p)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *body = ir_func_add_block(f, "body");
    IrBlock *merge = ir_func_add_block(f, "merge");
    *v = ir_alloc_vreg(m, 8, true, false);
    *p = ir_alloc_vreg(m, 8, true, false);
    ir_emit_br(entry, body->label);
    vec_push(body->preds, entry);
    ir_emit_binop(body, OP_ADD, *v, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_br(body, merge->label);
    vec_push(merge->preds, body);
    IrInstr *phi = ir_emit_phi_at_start(merge, *p, 1);
    ir_phi_add_entry(phi, ir_operand_vreg(*v), body);
    ir_emit_ret(merge, ir_operand_vreg(*p));
    return m;
}

static IrModule *build_many_crossing_call(Arena *a, u32 *vals, u32 n)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    for (u32 i = 0; i < n; i++)
    {
        vals[i] = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, vals[i], ir_operand_imm((i64) i + 1), ir_operand_imm(3));
    }
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_call(entry, r, "foo", 0, NULL);
    u32 acc = r;
    for (u32 i = 0; i < n; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(vals[i]));
        u32 next2 = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next2, ir_operand_vreg(next), ir_operand_vreg(vals[i]));
        acc = next2;
    }
    ir_emit_ret(entry, ir_operand_vreg(acc));
    return m;
}

static IrModule *build_cross_block_crossing(Arena *a, u32 *v)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *body = ir_func_add_block(f, "body");
    u32 dummy[5];
    for (u32 i = 0; i < 5; i++)
    {
        dummy[i] = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, dummy[i], ir_operand_imm((i64) i + 1), ir_operand_imm(2));
    }
    *v = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *v, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_br(entry, body->label);
    vec_push(body->preds, entry);
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_call(body, r, "foo", 0, NULL);
    u32 out = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(body, OP_ADD, out, ir_operand_vreg(*v), ir_operand_vreg(r));
    u32 out2 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(body, OP_ADD, out2, ir_operand_vreg(out), ir_operand_vreg(*v));
    u32 acc = out2;
    for (u32 i = 0; i < 5; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(body, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(dummy[i]));
        acc = next;
    }
    ir_emit_ret(body, ir_operand_vreg(acc));
    return m;
}

static IrModule *build_loop_crossing(Arena *a, u32 *v)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    IrBlock *head = ir_func_add_block(f, "head");
    IrBlock *exit = ir_func_add_block(f, "exit");
    u32 dummy[5];
    for (u32 i = 0; i < 5; i++)
    {
        dummy[i] = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, dummy[i], ir_operand_imm((i64) i + 1), ir_operand_imm(2));
    }
    u32 cond = ir_alloc_vreg(m, 8, false, false);
    *v = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ICMP_SLT, cond, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, *v, ir_operand_imm(3), ir_operand_imm(4));
    ir_emit_br(entry, head->label);
    vec_push(head->preds, entry);
    vec_push(head->preds, head);
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_call(head, r, "foo", 0, NULL);
    u32 out = ir_alloc_vreg(m, 8, true, false);
    u32 out2 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(head, OP_ADD, out, ir_operand_vreg(*v), ir_operand_vreg(r));
    ir_emit_binop(head, OP_ADD, out2, ir_operand_vreg(out), ir_operand_vreg(*v));
    ir_emit_brcond(head, ir_operand_vreg(cond), head->label, exit->label);
    vec_push(exit->preds, head);
    u32 acc = out2;
    for (u32 i = 0; i < 5; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(exit, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(dummy[i]));
        acc = next;
    }
    ir_emit_ret(exit, ir_operand_vreg(acc));
    return m;
}

static IrModule *build_two_sided_split(Arena *a, u32 *target)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 dummy[5];
    for (u32 i = 0; i < 5; i++)
    {
        dummy[i] = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, dummy[i], ir_operand_imm((i64) i + 1), ir_operand_imm(2));
    }
    *target = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *target, ir_operand_imm(7), ir_operand_imm(8));
    u32 pre1 = ir_alloc_vreg(m, 8, true, false);
    u32 pre2 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, pre1, ir_operand_vreg(*target), ir_operand_vreg(*target));
    ir_emit_binop(entry, OP_ADD, pre2, ir_operand_vreg(pre1), ir_operand_vreg(*target));
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_call(entry, r, "foo", 0, NULL);
    u32 post1 = ir_alloc_vreg(m, 8, true, false);
    u32 post2 = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, post1, ir_operand_vreg(*target), ir_operand_vreg(r));
    ir_emit_binop(entry, OP_ADD, post2, ir_operand_vreg(post1), ir_operand_vreg(*target));
    u32 acc = post2;
    for (u32 i = 0; i < 5; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(dummy[i]));
        acc = next;
    }
    ir_emit_ret(entry, ir_operand_vreg(acc));
    return m;
}

static IrModule *build_hot_eviction(Arena *a, u32 *hot, u32 *cold)
{
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    *hot = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *hot, ir_operand_imm(1), ir_operand_imm(2));
    u32 fill[11];
    for (u32 i = 0; i < 11; i++)
    {
        fill[i] = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, fill[i], ir_operand_imm((i64) i + 3), ir_operand_imm(0));
    }
    *cold = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, *cold, ir_operand_imm(0), ir_operand_imm(0));
    u32 acc = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, acc, ir_operand_vreg(*cold), ir_operand_imm(0));
    for (u32 i = 0; i < 11; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(fill[i]));
        acc = next;
    }
    for (u32 i = 0; i < 5; i++)
    {
        u32 next = ir_alloc_vreg(m, 8, true, false);
        ir_emit_binop(entry, OP_ADD, next, ir_operand_vreg(acc), ir_operand_vreg(*hot));
        acc = next;
    }
    ir_emit_ret(entry, ir_operand_vreg(acc));
    return m;
}

static u32 count_spilled(const RegAllocation *alloc, const LiveIntervals *set)
{
    u32 spilled = 0;
    for (u32 i = 0; i < set->n; i++)
    {
        if (alloc->phys_map[set->ivs[i].vreg] < 0)
        {
            spilled++;
        }
    }
    return spilled;
}
static u32 segment_count(const RegAllocation *alloc, u32 vreg)
{
    return alloc->seg_begin[vreg + 1] - alloc->seg_begin[vreg];
}

TEST(regalloc, overlapping_intervals_never_share_a_register)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    for (u32 i = 0; i < set.n; i++)
    {
        for (u32 j = i + 1; j < set.n; j++)
        {
            const LiveInterval *u = &set.ivs[i];
            const LiveInterval *v = &set.ivs[j];
            /* Strict overlap: a def may reuse the register of a value that dies
               at exactly that instruction (the two-address/coalescing case). */
            bool overlap = u->start < v->end && v->start < u->end;
            int ru = alloc->phys_map[u->vreg];
            int rv = alloc->phys_map[v->vreg];
            if (overlap && ru >= 0 && rv >= 0)
            {
                EXPECT_TRUE(ru != rv);
            }
        }
    }
    arena_free(a);
}

TEST(regalloc, assigns_the_lowest_free_register)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_EQ(alloc->phys_map[x], R_ECX);
    EXPECT_EQ(alloc->frame_size, 0);
    EXPECT_EQ(alloc->saved_mask, 0);
    arena_free(a);
}

TEST(regalloc, coalesces_a_dying_operand_register)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* The def-use chain coalesces: each value reuses the register of the operand
       that dies at its definition. */
    EXPECT_EQ(alloc->phys_map[y], alloc->phys_map[x]);
    EXPECT_EQ(alloc->phys_map[z], alloc->phys_map[y]);
    arena_free(a);
}

TEST(regalloc, loc_at_reports_a_register)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    RegLoc lx = loc_at(alloc, ir_operand_vreg(x), 0);
    EXPECT_EQ(lx.kind, LOC_REG);
    EXPECT_EQ(lx.reg, R_ECX);
    arena_free(a);
}

TEST(regalloc, loc_at_reports_an_immediate)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    RegLoc li = loc_at(alloc, ir_operand_imm(7), 0);
    EXPECT_EQ(li.kind, LOC_IMM);
    arena_free(a);
}

TEST(regalloc, loc_at_is_stable_across_a_single_segment)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, x);
    EXPECT_EQ(alloc->seg_begin[x + 1] - alloc->seg_begin[x], 1u);
    RegLoc at_start = loc_at(alloc, ir_operand_vreg(x), iv->start);
    RegLoc at_mid = loc_at(alloc, ir_operand_vreg(x), (iv->start + iv->end) / 2);
    RegLoc at_end = loc_at(alloc, ir_operand_vreg(x), iv->end);
    EXPECT_EQ(at_start.kind, LOC_REG);
    EXPECT_EQ(at_start.reg, at_mid.reg);
    EXPECT_EQ(at_mid.reg, at_end.reg);
    arena_free(a);
}

#ifndef NDEBUG
TEST(regalloc, loc_at_out_of_range_is_an_internal_error)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, x);
    pid_t pid = fork();
    if (pid == 0)
    {
        freopen("/dev/null", "w", stderr);
        loc_at(alloc, ir_operand_vreg(x), iv->end + 1000u);
        _exit(0);
    }
    ASSERT(pid > 0);
    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid);
    EXPECT_TRUE(WIFSIGNALED(status));
    if (WIFSIGNALED(status))
    {
        EXPECT_EQ(WTERMSIG(status), SIGABRT);
    }
    arena_free(a);
}
#endif

TEST(regalloc, x87_values_are_always_spilled)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    u32 ld = ir_alloc_fp_vreg(m, 16);
    IrParam *p = arena_alloc(a, sizeof(IrParam), _Alignof(IrParam));
    p->name = "ld";
    p->type = type_long_double();
    p->vreg = ld;
    vec_push(f->params, p);
    ir_func_add_block(f, "entry");
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_NOTNULL(find_iv(&set, ld));
    EXPECT_EQ(find_iv(&set, ld)->cls, RC_X87);
    EXPECT_EQ(alloc->phys_map[ld], -1);
    EXPECT_TRUE(alloc->slot_map[ld] > 0);
    EXPECT_TRUE(alloc->frame_size >= 16);
    arena_free(a);
}

TEST(regalloc, call_crossing_gpr_uses_a_callee_saved_register)
{
    Arena *a = arena_new();
    u32 live, call_result;
    IrModule *m = build_crossing_call(a, &live, &call_result);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    int reg = alloc->phys_map[live];
    EXPECT_TRUE(reg >= 0);
    EXPECT_TRUE(is_callee_saved((u8) reg));
    EXPECT_TRUE((alloc->saved_mask & 1u) == 1u);
    arena_free(a);
}

TEST(regalloc, call_crossing_xmm_splits_at_the_call)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 d = ir_alloc_fp_vreg(m, 8);
    u32 e = ir_alloc_fp_vreg(m, 8);
    u32 e2 = ir_alloc_fp_vreg(m, 8);
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_FADD, d, ir_operand_imm(0), ir_operand_imm(0));
    ir_emit_call(entry, r, "foo", 0, NULL);
    ir_emit_binop(entry, OP_FADD, e, ir_operand_vreg(d), ir_operand_imm(0));
    ir_emit_binop(entry, OP_FADD, e2, ir_operand_vreg(e), ir_operand_vreg(d));
    ir_emit_ret(entry, ir_operand_imm(0));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, d);
    EXPECT_EQ(iv->cls, RC_XMM);
    /* No XMM lane is callee-saved, so the value is split at the call and rides
       caller-saved lanes with a store/reload gap. */
    EXPECT_EQ(alloc->seg_begin[d + 1] - alloc->seg_begin[d], 2u);
    EXPECT_EQ(alloc->ncall_gaps, 1u);
    EXPECT_TRUE(alloc->has_slot[d]);
    EXPECT_EQ(loc_at(alloc, ir_operand_vreg(d), iv->start).kind, LOC_REG);
    EXPECT_EQ(loc_at(alloc, ir_operand_vreg(d), iv->end).kind, LOC_REG);
    arena_free(a);
}

TEST(regalloc, overfull_callee_saved_bank_splits_the_excess)
{
    Arena *a = arena_new();
    u32 vals[7];
    IrModule *m = build_many_crossing_call(a, vals, 7);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* Five callee-saved GPRs take the first five values whole; the last two
       split at the call. */
    EXPECT_EQ(alloc->saved_mask, 0x3Du);
    EXPECT_EQ(alloc->ncall_gaps, 2u);
    u32 split = vals[6];
    EXPECT_EQ(alloc->seg_begin[split + 1] - alloc->seg_begin[split], 2u);
    EXPECT_TRUE(alloc->has_slot[split]);
    const LiveInterval *iv = find_iv(&set, split);
    RegLoc pre = loc_at(alloc, ir_operand_vreg(split), iv->start);
    RegLoc post = loc_at(alloc, ir_operand_vreg(split), iv->end);
    EXPECT_EQ(pre.kind, LOC_REG);
    EXPECT_EQ(post.kind, LOC_REG);
    EXPECT_FALSE(is_callee_saved(pre.reg));
    EXPECT_FALSE(is_callee_saved(post.reg));
    arena_free(a);
}

/* Instruction positions do not order blocks, so a cross-block value cannot split at a call. */
TEST(regalloc, cross_block_crossing_value_keeps_one_home)
{
    Arena *a = arena_new();
    u32 v;
    IrModule *m = build_cross_block_crossing(a, &v);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, v);
    EXPECT_EQ(alloc->seg_begin[v + 1] - alloc->seg_begin[v], 1u);
    EXPECT_EQ(alloc->ncall_gaps, 0u);
    RegLoc pre = loc_at(alloc, ir_operand_vreg(v), iv->start);
    RegLoc post = loc_at(alloc, ir_operand_vreg(v), iv->end);
    EXPECT_EQ(pre.kind, post.kind);
    if (pre.kind == LOC_REG)
    {
        EXPECT_EQ(pre.reg, post.reg);
    }
    arena_free(a);
}

TEST(regalloc, call_split_is_rejected_when_a_back_edge_straddles_it)
{
    Arena *a = arena_new();
    u32 v;
    IrModule *m = build_loop_crossing(a, &v);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, v);
    EXPECT_EQ(alloc->seg_begin[v + 1] - alloc->seg_begin[v], 1u);
    EXPECT_EQ(alloc->ncall_gaps, 0u);
    RegLoc pre = loc_at(alloc, ir_operand_vreg(v), iv->start);
    RegLoc post = loc_at(alloc, ir_operand_vreg(v), iv->end);
    EXPECT_EQ(pre.kind, post.kind);
    if (pre.kind == LOC_REG)
    {
        EXPECT_EQ(pre.reg, post.reg);
    }
    arena_free(a);
}

TEST(regalloc, split_value_read_on_both_sides_of_a_call)
{
    Arena *a = arena_new();
    u32 target;
    IrModule *m = build_two_sided_split(a, &target);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    const LiveInterval *iv = find_iv(&set, target);
    EXPECT_EQ(alloc->seg_begin[target + 1] - alloc->seg_begin[target], 2u);
    EXPECT_TRUE(alloc->has_slot[target]);
    EXPECT_EQ(alloc->ncall_gaps, 1u);
    RegLoc pre = loc_at(alloc, ir_operand_vreg(target), iv->start);
    RegLoc post = loc_at(alloc, ir_operand_vreg(target), iv->end);
    EXPECT_EQ(pre.kind, LOC_REG);
    EXPECT_EQ(post.kind, LOC_REG);
    EXPECT_FALSE(is_callee_saved(pre.reg));
    EXPECT_FALSE(is_callee_saved(post.reg));
    arena_free(a);
}

TEST(regalloc, split_subrange_falls_back_to_a_slot_under_pressure)
{
    Arena *a = arena_new();
    u32 vals[16];
    IrModule *m = build_many_crossing_call(a, vals, 16);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    bool slot_run = false;
    for (u32 i = 0; i < 16; i++)
    {
        for (u32 s = alloc->seg_begin[vals[i]]; s < alloc->seg_begin[vals[i] + 1]; s++)
        {
            if (alloc->segments[s].kind == SEG_MEM)
            {
                slot_run = true;
            }
        }
    }
    EXPECT_TRUE(alloc->ncall_gaps > 0u);
    EXPECT_TRUE(slot_run);
    arena_free(a);
}

TEST(regalloc, pressure_split_reloads_at_the_next_use)
{
    Arena *a = arena_new();
    IrModule *m = build_pressure(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* A value that is longer than every active interval cannot evict one, so
       it spills up to its next use and then rides a freed register. */
    EXPECT_TRUE(alloc->nseg_gaps > 0u);
    u32 reloaded = 0;
    for (u32 v = 0; v < alloc->nvregs; v++)
    {
        if (segment_count(alloc, v) < 2)
        {
            continue;
        }
        u32 first = alloc->seg_begin[v];
        EXPECT_EQ(alloc->segments[first].kind, SEG_MEM);
        RegLoc tail = loc_at(alloc, ir_operand_vreg(v), alloc->segments[first + 1].start);
        EXPECT_EQ(tail.kind, LOC_REG);
        reloaded++;
    }
    EXPECT_TRUE(reloaded >= 1u);
    arena_free(a);
}

TEST(regalloc, eviction_never_spills_the_hottest_active_value)
{
    Arena *a = arena_new();
    u32 hot, cold;
    IrModule *m = build_hot_eviction(a, &hot, &cold);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* `hot` is defined first and read most; when `cold` cannot get a register
       the allocator takes a colder value's register instead of `hot`'s. */
    EXPECT_TRUE(alloc->phys_map[hot] >= 0);
    EXPECT_TRUE(count_spilled(alloc, &set) >= 1u);
    arena_free(a);
}

TEST(regalloc, pressure_segments_never_overlap_in_a_register)
{
    Arena *a = arena_new();
    IrModule *m = build_pressure(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    for (u32 u = 0; u < alloc->nvregs; u++)
    {
        const LiveInterval *iu = find_iv(&set, u);
        for (u32 v = u + 1; v < alloc->nvregs; v++)
        {
            const LiveInterval *iv = find_iv(&set, v);
            if (!iu || !iv || iu->cls != iv->cls)
            {
                continue;
            }
            for (u32 su = alloc->seg_begin[u]; su < alloc->seg_begin[u + 1]; su++)
            {
                if (alloc->segments[su].kind != SEG_REG)
                {
                    continue;
                }
                for (u32 sv = alloc->seg_begin[v]; sv < alloc->seg_begin[v + 1]; sv++)
                {
                    if (alloc->segments[sv].kind != SEG_REG)
                    {
                        continue;
                    }
                    bool overlap = alloc->segments[su].start < alloc->segments[sv].end &&
                                   alloc->segments[sv].start < alloc->segments[su].end;
                    if (overlap)
                    {
                        EXPECT_TRUE(alloc->segments[su].reg != alloc->segments[sv].reg);
                    }
                }
            }
        }
    }
    arena_free(a);
}

TEST(regalloc, split_segments_cover_the_live_interval_without_gaps)
{
    Arena *a = arena_new();
    u32 vals[16];
    IrModule *m = build_many_crossing_call(a, vals, 16);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    for (u32 i = 0; i < set.n; i++)
    {
        u32 v = set.ivs[i].vreg;
        u32 lo = alloc->seg_begin[v];
        u32 hi = alloc->seg_begin[v + 1];
        EXPECT_TRUE(lo < hi);
        EXPECT_EQ(alloc->segments[lo].start, set.ivs[i].start);
        EXPECT_EQ(alloc->segments[hi - 1].end, set.ivs[i].end);
        for (u32 s = lo; s < hi; s++)
        {
            EXPECT_TRUE(alloc->segments[s].start <= alloc->segments[s].end);
            if (s > lo)
            {
                EXPECT_EQ(alloc->segments[s].start, alloc->segments[s - 1].end + 1);
            }
        }
    }
    arena_free(a);
}

TEST(regalloc, pressure_beyond_the_bank_spills)
{
    Arena *a = arena_new();
    IrModule *m = build_pressure(a);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_TRUE(count_spilled(alloc, &set) >= 3);
    arena_free(a);
}

TEST(regalloc, reserved_registers_are_never_allocated)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 s = ir_alloc_vreg(m, 8, true, false);
    u32 q = ir_alloc_vreg(m, 8, true, false);
    u32 t = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_SHL, s, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_SDIV, q, ir_operand_imm(100), ir_operand_vreg(s));
    ir_emit_binop(entry, OP_ADD, t, ir_operand_vreg(q), ir_operand_vreg(s));
    ir_emit_ret(entry, ir_operand_vreg(t));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    for (u32 i = 0; i < set.n; i++)
    {
        int reg = alloc->phys_map[set.ivs[i].vreg];
        if (reg >= 0)
        {
            EXPECT_FALSE(is_reserved((u8) reg));
        }
    }
    arena_free(a);
}

TEST(regalloc, variable_shift_avoids_count_register)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 cnt = ir_alloc_vreg(m, 8, true, false);
    u32 s = ir_alloc_vreg(m, 8, true, false);
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, cnt, ir_operand_imm(1), ir_operand_imm(0));
    ir_emit_binop(entry, OP_SHL, s, ir_operand_imm(8), ir_operand_vreg(cnt));
    ir_emit_binop(entry, OP_ADD, r, ir_operand_vreg(s), ir_operand_vreg(cnt));
    ir_emit_ret(entry, ir_operand_vreg(r));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* `cnt` is live across the shift, so it must not ride %rcx: lowering moves
       it into the count register, which has to be free at that point. */
    EXPECT_TRUE((u8) alloc->phys_map[cnt] != R_ECX);
    arena_free(a);
}

TEST(regalloc, division_avoids_high_half)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 d = ir_alloc_vreg(m, 8, true, false);
    u32 q = ir_alloc_vreg(m, 8, true, false);
    u32 r = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, d, ir_operand_imm(100), ir_operand_imm(0));
    ir_emit_binop(entry, OP_SDIV, q, ir_operand_vreg(d), ir_operand_imm(3));
    ir_emit_binop(entry, OP_ADD, r, ir_operand_vreg(q), ir_operand_vreg(d));
    ir_emit_ret(entry, ir_operand_vreg(r));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* %rdx is written by cdq; the dividend (live into the divide) and the
       quotient must avoid it. */
    EXPECT_TRUE((u8) alloc->phys_map[d] != R_EDX);
    EXPECT_TRUE((u8) alloc->phys_map[q] != R_EDX);
    arena_free(a);
}

TEST(regalloc, xmm_bank_uses_allocatable_lanes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 d = ir_alloc_fp_vreg(m, 8);
    u32 e = ir_alloc_fp_vreg(m, 8);
    ir_emit_binop(entry, OP_FADD, d, ir_operand_imm(0), ir_operand_imm(0));
    ir_emit_binop(entry, OP_FADD, e, ir_operand_vreg(d), ir_operand_imm(0));
    ir_emit_ret(entry, ir_operand_imm(0));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* xmm0/1 are lowering scratch and stay reserved; xmm2-15 allocate. */
    EXPECT_EQ(alloc->phys_map[d], 2);
    EXPECT_TRUE(alloc->phys_map[e] >= 2 && alloc->phys_map[e] < 16);
    arena_free(a);
}

TEST(regalloc, call_operands_avoid_argument_lanes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 x = ir_alloc_vreg(m, 8, true, false);
    u32 y = ir_alloc_vreg(m, 8, true, false);
    u32 res = ir_alloc_vreg(m, 8, true, false);
    u32 sum = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, x, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, y, ir_operand_imm(3), ir_operand_imm(4));
    IrOperand args[2] = {ir_operand_vreg(x), ir_operand_vreg(y)};
    ir_emit_call(entry, res, "foo", 2, args);
    ir_emit_binop(entry, OP_ADD, sum, ir_operand_vreg(res), ir_operand_imm(0));
    ir_emit_ret(entry, ir_operand_vreg(sum));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_FALSE(is_arg_lane((u8) alloc->phys_map[x]));
    EXPECT_FALSE(is_arg_lane((u8) alloc->phys_map[y]));
    arena_free(a);
}

TEST(regalloc, parameters_avoid_argument_lanes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 p = ir_alloc_vreg(m, 8, true, false);
    IrParam *param = arena_alloc(a, sizeof(IrParam), _Alignof(IrParam));
    param->name = "p";
    param->type = type_int();
    param->vreg = p;
    vec_push(f->params, param);
    u32 sum = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, sum, ir_operand_vreg(p), ir_operand_imm(1));
    ir_emit_ret(entry, ir_operand_vreg(sum));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_FALSE(is_arg_lane((u8) alloc->phys_map[p]));
    arena_free(a);
}

TEST(regalloc, phi_result_coalesces_with_a_dying_predecessor_operand)
{
    Arena *a = arena_new();
    u32 v, p;
    IrModule *m = build_phi_chain(a, &v, &p);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* v dies at the edge copy, so the phi copy is a self-move: p reuses v. */
    EXPECT_TRUE(alloc->phys_map[v] >= 0);
    EXPECT_EQ(alloc->phys_map[p], alloc->phys_map[v]);
    arena_free(a);
}

TEST(regalloc, scalar_gp_arguments_ride_their_lanes)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 x = ir_alloc_vreg(m, 8, true, false);
    u32 y = ir_alloc_vreg(m, 8, true, false);
    u32 res = ir_alloc_vreg(m, 8, true, false);
    u32 sum = ir_alloc_vreg(m, 8, true, false);
    ir_emit_binop(entry, OP_ADD, x, ir_operand_imm(1), ir_operand_imm(2));
    ir_emit_binop(entry, OP_ADD, y, ir_operand_imm(3), ir_operand_imm(4));
    IrOperand args[2] = {ir_operand_vreg(x), ir_operand_vreg(y)};
    IrInstr *call = ir_emit_call(entry, res, "foo", 2, args);
    Type *types[2] = {type_int(), type_int()};
    ir_call_set_types(call, types, type_int());
    ir_emit_binop(entry, OP_ADD, sum, ir_operand_vreg(res), ir_operand_imm(0));
    ir_emit_ret(entry, ir_operand_vreg(sum));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* A record-free call's scalar GP arguments are born in the lane they pass
       in, so lowering emits no argument moves. */
    EXPECT_EQ(alloc->phys_map[x], R_EDI);
    EXPECT_EQ(alloc->phys_map[y], R_ESI);
    arena_free(a);
}

TEST(regalloc, alloca_results_are_rematerialized)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 p = ir_alloc_vreg(m, 8, true, false);
    u32 x = ir_alloc_vreg(m, 8, true, false);
    ir_emit_alloca(entry, p, 32);
    ir_emit_load(entry, x, ir_operand_vreg(p), false);
    ir_emit_ret(entry, ir_operand_vreg(x));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    /* A static alloca's address is a cheap %rbp-relative lea, so it is never
       given a register or a spill slot; lowering recomputes it at each use. */
    EXPECT_TRUE(regalloc_is_remat(alloc, p));
    EXPECT_EQ(alloc->phys_map[p], -1);
    EXPECT_EQ(segment_count(alloc, p), 1u);
    EXPECT_EQ(alloc->segments[alloc->seg_begin[p]].kind, SEG_REMAT);
    regalloc_set_remat_disp(alloc, p, -24);
    EXPECT_EQ(regalloc_remat_disp(alloc, p), -24);
    RegLoc rl = loc_at(alloc, ir_operand_vreg(p), 0);
    EXPECT_EQ(rl.kind, LOC_REMAT);
    EXPECT_EQ(rl.disp, -24);
    arena_free(a);
}

TEST(regalloc, constant_index_gep_over_alloca_is_rematerialized)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *entry = ir_func_add_block(f, "entry");
    u32 p = ir_alloc_vreg(m, 8, true, false);
    u32 q = ir_alloc_vreg(m, 8, true, false);
    u32 x = ir_alloc_vreg(m, 8, true, false);
    ir_emit_alloca(entry, p, 32);
    ir_emit_gep(entry, q, ir_operand_vreg(p), ir_operand_imm(2), 4);
    ir_emit_load(entry, x, ir_operand_vreg(q), false);
    ir_emit_ret(entry, ir_operand_vreg(x));
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_linear(f, &set, x86_64_target(), a);
    EXPECT_TRUE(regalloc_is_remat(alloc, q));
    EXPECT_EQ(alloc->phys_map[q], -1);
    EXPECT_EQ(alloc->segments[alloc->seg_begin[q]].kind, SEG_REMAT);
    EXPECT_EQ(loc_at(alloc, ir_operand_vreg(q), 2).kind, LOC_REMAT);
    arena_free(a);
}

TEST(regalloc, allocation_is_deterministic)
{
    Arena *a1 = arena_new();
    Arena *a2 = arena_new();
    IrModule *m1 = build_pressure(a1);
    IrModule *m2 = build_pressure(a2);
    IrFunction *f1 = (IrFunction *) vec_get(m1->funcs, 0);
    IrFunction *f2 = (IrFunction *) vec_get(m2->funcs, 0);
    LiveIntervals s1 = liveinterval_compute(f1, m1, a1);
    LiveIntervals s2 = liveinterval_compute(f2, m2, a2);
    RegAllocation *r1 = regalloc_linear(f1, &s1, x86_64_target(), a1);
    RegAllocation *r2 = regalloc_linear(f2, &s2, x86_64_target(), a2);
    EXPECT_EQ(r1->frame_size, r2->frame_size);
    EXPECT_EQ(r1->saved_mask, r2->saved_mask);
    EXPECT_EQ(r1->nvregs, r2->nvregs);
    for (u32 v = 0; v < r1->nvregs; v++)
    {
        EXPECT_EQ(r1->phys_map[v], r2->phys_map[v]);
        EXPECT_EQ(r1->slot_map[v], r2->slot_map[v]);
    }
    arena_free(a1);
    arena_free(a2);
}

TEST(regalloc, all_spilled_puts_every_value_in_a_slot)
{
    Arena *a = arena_new();
    u32 x, y, z;
    IrModule *m = build_chain(a, &x, &y, &z);
    IrFunction *f = (IrFunction *) vec_get(m->funcs, 0);
    LiveIntervals set = liveinterval_compute(f, m, a);
    RegAllocation *alloc = regalloc_all_spilled(f, &set, a);
    for (u32 i = 0; i < set.n; i++)
    {
        u32 v = set.ivs[i].vreg;
        EXPECT_EQ(alloc->phys_map[v], -1);
        EXPECT_TRUE(alloc->has_slot[v]);
        for (u32 s = alloc->seg_begin[v]; s < alloc->seg_begin[v + 1]; s++)
        {
            EXPECT_EQ(alloc->segments[s].kind, SEG_MEM);
        }
    }
    EXPECT_TRUE(alloc->frame_size > 0);
    arena_free(a);
}
