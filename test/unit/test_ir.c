#include "harness.h"
#include "ir.h"
#include "util/arena.h"

TEST(ir, module_creation)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    EXPECT_NOTNULL(m);
    EXPECT_EQ(m->next_vreg, 0);
    EXPECT_EQ(m->width_count, 0);
    EXPECT_NOTNULL(m->funcs);
    EXPECT_NOTNULL(m->globals);
    arena_free(a);
}

TEST(ir, vreg_allocation)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);

    u32 v0 = ir_alloc_vreg(m, 4, true, false);
    u32 v1 = ir_alloc_vreg(m, 8, true, false);
    u32 v2 = ir_alloc_vreg(m, 1, true, false);

    EXPECT_EQ(v0, 0);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(v2, 2);
    EXPECT_EQ(m->width_count, 3);
    EXPECT_EQ(m->widths[0], 4);
    EXPECT_EQ(m->widths[1], 8);
    EXPECT_EQ(m->widths[2], 1);

    arena_free(a);
}

TEST(ir, vreg_table_growth)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);

    for (int i = 0; i < 20; i++)
    {
        ir_alloc_vreg(m, (u8) (i + 1), true, false);
    }

    EXPECT_EQ(m->width_count, 20);
    for (int i = 0; i < 20; i++)
    {
        EXPECT_EQ(m->widths[i], (u8) (i + 1));
    }

    arena_free(a);
}

TEST(ir, floatness_value_class)
{
    /* floatness rides parallel to widths/signedness; ir_alloc_fp_vreg sets it. */
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    u32 fi = ir_alloc_fp_vreg(m, 4);
    u32 di = ir_alloc_fp_vreg(m, 8);
    u32 ii = ir_alloc_vreg(m, 4, true, false);

    EXPECT_EQ(fi, 0);
    EXPECT_EQ(di, 1);
    EXPECT_EQ(ii, 2);
    EXPECT_TRUE(ir_vreg_float(m, fi));
    EXPECT_TRUE(ir_vreg_float(m, di));
    EXPECT_FALSE(ir_vreg_float(m, ii));
    EXPECT_FALSE(ir_vreg_signed(m, fi));
    EXPECT_EQ(m->widths[fi], 4);
    EXPECT_EQ(m->widths[di], 8);
    EXPECT_EQ(m->floatness[fi], true);
    EXPECT_EQ(m->floatness[di], true);
    EXPECT_EQ(m->floatness[ii], false);
    arena_free(a);
}

TEST(ir, fp_conversion_unary_opcodes)
{
    /* ITOF/FTOI/FCONV are unary opcodes, emitted like the integer extends. */
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    u32 src = ir_alloc_vreg(m, 4, true, false);
    u32 dst = ir_alloc_fp_vreg(m, 8);

    IrInstr *itof = ir_emit_unary(bb, OP_ITOF, dst, ir_operand_vreg(src));
    EXPECT_EQ(itof->opcode, OP_ITOF);
    EXPECT_EQ(itof->nops, 1);
    EXPECT_EQ(itof->ops[0].u.vreg, src);
    IrInstr *ftoi = ir_emit_unary(bb, OP_FTOI, src, ir_operand_vreg(dst));
    EXPECT_EQ(ftoi->opcode, OP_FTOI);
    IrInstr *fconv = ir_emit_unary(bb, OP_FCONV, dst, ir_operand_vreg(dst));
    EXPECT_EQ(fconv->opcode, OP_FCONV);
    EXPECT_STR_EQ(ir_opcode_name(OP_ITOF), "OP_ITOF");
    EXPECT_STR_EQ(ir_opcode_name(OP_FCONV), "OP_FCONV");
    arena_free(a);
}

TEST(ir, function_and_block)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    EXPECT_NOTNULL(f);
    EXPECT_STR_EQ(f->name, "main");
    EXPECT_EQ(vec_size(f->blocks), 1);
    EXPECT_NOTNULL(bb);
    EXPECT_STR_EQ(bb->label, "entry");

    arena_free(a);
}

TEST(ir, emit_ret)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    IrInstr *i = ir_emit_ret(bb, ir_operand_imm(42));
    EXPECT_NOTNULL(i);
    EXPECT_EQ(i->opcode, OP_RET);
    EXPECT_EQ(i->nops, 1);
    EXPECT_TRUE(i->ops[0].is_imm);
    EXPECT_EQ(i->ops[0].u.imm, 42);

    arena_free(a);
}

TEST(ir, emit_unreachable)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");

    IrInstr *i = ir_emit_unreachable(bb);
    EXPECT_NOTNULL(i);
    EXPECT_EQ(i->opcode, OP_UNREACHABLE);
    EXPECT_EQ(i->nops, 0);

    arena_free(a);
}

TEST(ir, operand_vreg)
{
    IrOperand o = ir_operand_vreg(7);
    EXPECT_FALSE(o.is_imm);
    EXPECT_EQ(o.u.vreg, 7);
}

TEST(ir, operand_imm)
{
    IrOperand o = ir_operand_imm(-123);
    EXPECT_TRUE(o.is_imm);
    EXPECT_EQ(o.u.imm, -123);
}

TEST(ir, opcode_names)
{
    EXPECT_STR_EQ(ir_opcode_name(OP_RET), "OP_RET");
    EXPECT_STR_EQ(ir_opcode_name(OP_ADD), "OP_ADD");
    EXPECT_STR_EQ(ir_opcode_name(OP_ICMP_SLT), "OP_ICMP_SLT");
}

TEST(ir, emit_ret_void)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_void());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrInstr *i = ir_emit_ret_void(bb);
    EXPECT_EQ(i->opcode, OP_RET);
    EXPECT_EQ(i->result, NO_VREG);
    EXPECT_EQ(i->nops, 0);
    arena_free(a);
}

TEST(ir, emit_add)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrInstr *i = ir_emit_binop(bb, OP_ADD, 0, ir_operand_vreg(1), ir_operand_imm(2));
    EXPECT_EQ(i->opcode, OP_ADD);
    EXPECT_EQ(i->result, 0);
    EXPECT_EQ(i->nops, 2);
    EXPECT_EQ(i->ops[0].u.vreg, 1);
    EXPECT_EQ(i->ops[1].u.imm, 2);
    arena_free(a);
}

TEST(ir, emit_call)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrOperand args[2] = {ir_operand_imm(1), ir_operand_imm(2)};
    IrInstr *i = ir_emit_call(bb, 0, "foo", 2, args);
    EXPECT_EQ(i->opcode, OP_CALL);
    EXPECT_EQ(i->result, 0);
    EXPECT_EQ(i->extra.call.nargs, 2);
    EXPECT_STR_EQ(i->extra.call.name, "foo");
    EXPECT_EQ(i->extra.call.args[0].u.imm, 1);
    arena_free(a);
}

TEST(ir, emit_br)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrInstr *i = ir_emit_br(bb, "target");
    EXPECT_EQ(i->opcode, OP_BR);
    EXPECT_STR_EQ(i->extra.br.target_label, "target");
    arena_free(a);
}

TEST(ir, emit_brcond)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrInstr *i = ir_emit_brcond(bb, ir_operand_vreg(0), "then", "else");
    EXPECT_EQ(i->opcode, OP_BRCOND);
    EXPECT_STR_EQ(i->extra.brcond.true_label, "then");
    EXPECT_STR_EQ(i->extra.brcond.false_label, "else");
    arena_free(a);
}

TEST(ir, emit_phi)
{
    Arena *a = arena_new();
    IrModule *m = ir_module_new(a);
    IrFunction *f = ir_module_add_func(m, "main", type_int());
    IrBlock *bb = ir_func_add_block(f, "entry");
    IrInstr *phi = ir_emit_phi(bb, 2, 2);
    EXPECT_EQ(phi->opcode, OP_PHI);
    EXPECT_EQ(phi->result, 2);
    EXPECT_EQ(phi->extra.phi.nentries, 2);
    ir_phi_add_entry(phi, ir_operand_imm(10), bb);
    ir_phi_add_entry(phi, ir_operand_imm(20), bb);
    EXPECT_EQ(phi->extra.phi.entries[0].val.u.imm, 10);
    EXPECT_EQ(phi->extra.phi.entries[1].val.u.imm, 20);
    arena_free(a);
}
