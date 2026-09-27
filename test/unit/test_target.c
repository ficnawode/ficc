#include "harness.h"
#include "target.h"
#include "x86_emit.h"

TEST(target, x86_64_name)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_TRUE(t && strcmp(t->name, "x86-64") == 0);
}

TEST(target, x86_64_gpr_bank_shape)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->gpr.cls, RC_GPR);
    EXPECT_EQ(t->gpr.num_regs, 16);
    EXPECT_EQ(t->gpr.names[0], R_EAX);
    EXPECT_EQ(t->gpr.names[7], R_EDI);
    EXPECT_EQ(t->gpr.names[15], R_R15);
    EXPECT_EQ(t->gpr.ncallee_saved, 6);
    EXPECT_EQ(t->gpr.callee_saved[0], R_EBX);
    EXPECT_EQ(t->gpr.callee_saved[1], R_EBP);
    EXPECT_EQ(t->gpr.callee_saved[5], R_R15);
}

TEST(target, x86_64_xmm_bank_shape)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->xmm.cls, RC_XMM);
    EXPECT_EQ(t->xmm.num_regs, 16);
    EXPECT_EQ(t->xmm.names[0], R_XMM0);
    EXPECT_EQ(t->xmm.names[7], 7);
    EXPECT_EQ(t->xmm.names[8], 8);
    EXPECT_EQ(t->xmm.names[15], 15);
    EXPECT_EQ(t->xmm.nfixed, 2); /* xmm0/1 are lowering scratch; xmm2-15 allocate */
}

TEST(target, x86_64_x87_bank_shape)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->x87.cls, RC_X87);
    EXPECT_TRUE(t->x87.memory_only);
}

TEST(target, x86_64_caller_saved_gprs_allocate)
{
    const TargetDesc *t = x86_64_target();
    const u8 allocatable[7] = {R_ECX, R_EDX, R_ESI, R_EDI, R_R8, R_R9, R_R10};
    for (u8 i = 0; i < 7; i++)
    {
        bool fixed = false;
        for (u8 f = 0; f < t->gpr.nfixed; f++)
        {
            if (t->gpr.fixed[f] == allocatable[i])
            {
                fixed = true;
            }
        }
        EXPECT_FALSE(fixed);
    }
    bool rax_fixed = false;
    bool r11_fixed = false;
    for (u8 f = 0; f < t->gpr.nfixed; f++)
    {
        if (t->gpr.fixed[f] == R_EAX)
        {
            rax_fixed = true;
        }
        if (t->gpr.fixed[f] == R_R11)
        {
            r11_fixed = true;
        }
    }
    EXPECT_TRUE(rax_fixed); /* %rax is the division/return scratch */
    EXPECT_TRUE(r11_fixed); /* %r11 stays the record/indirect-call scratch */
}

TEST(target, x86_64_sdiv_clobbers_rdx)
{
    const TargetDesc *t = x86_64_target();
    IrInstr in = {0};
    in.opcode = OP_SDIV;
    EXPECT_EQ(t->instr_clobbers(t, &in), (u16) (1u << R_EDX));
}

TEST(target, x86_64_variable_shift_clobbers_rcx)
{
    const TargetDesc *t = x86_64_target();
    IrInstr in = {0};
    in.opcode = OP_SHL;
    in.ops[1].is_imm = true;
    EXPECT_EQ(t->instr_clobbers(t, &in), 0); /* a constant count uses no %cl */
    in.ops[1].is_imm = false;
    EXPECT_EQ(t->instr_clobbers(t, &in), (u16) (1u << R_ECX));
}

TEST(target, x86_64_memcpy_clobbers_scratch)
{
    const TargetDesc *t = x86_64_target();
    IrInstr in = {0};
    in.opcode = OP_MEMCPY;
    EXPECT_EQ(t->instr_clobbers(t, &in), (u16) ((1u << R_ESI) | (1u << R_EDI) | (1u << R_ECX)));
}

TEST(target, x86_64_plain_binop_clobbers_nothing)
{
    const TargetDesc *t = x86_64_target();
    IrInstr in = {0};
    in.opcode = OP_ADD;
    EXPECT_EQ(t->instr_clobbers(t, &in), 0);
}

TEST(target, x86_64_gp_arg_registers)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->ngp, 6);
    EXPECT_EQ(t->gp_args[0], R_EDI);
    EXPECT_EQ(t->gp_args[1], R_ESI);
    EXPECT_EQ(t->gp_args[2], R_EDX);
    EXPECT_EQ(t->gp_args[3], R_ECX);
    EXPECT_EQ(t->gp_args[4], R_R8);
    EXPECT_EQ(t->gp_args[5], R_R9);
}

TEST(target, x86_64_fp_arg_registers)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->nfp, 8);
    for (u8 i = 0; i < t->nfp; i++)
    {
        EXPECT_EQ(t->fp_args[i], i); /* XMM0..XMM7 are the first eight lanes */
    }
}

TEST(target, x86_64_call_clobber_set)
{
    const TargetDesc *t = x86_64_target();
    bool has_rbx = false;
    for (u8 i = 0; i < t->nclobbered_call; i++)
    {
        if (t->clobbered_call[i] == R_EBX)
        {
            has_rbx = true;
        }
    }
    EXPECT_FALSE(has_rbx); /* callee-saved regs are never in the clobber set */
    EXPECT_EQ(t->clobbered_call[0], R_EAX);
}

TEST(target, x86_64_frame_numbers)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->word_width, 8);
    EXPECT_EQ(t->frame_align, 16);
    EXPECT_EQ(t->spill_align[1], 1);
    EXPECT_EQ(t->spill_align[2], 2);
    EXPECT_EQ(t->spill_align[4], 4);
    EXPECT_EQ(t->spill_align[8], 8);
    EXPECT_EQ(t->spill_align[16], 16);
}

TEST(target, x86_64_frame_register)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->frame_reg, R_EBP);
}

TEST(target, x86_64_gpr_return_register)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->return_reg(t, 8, RC_GPR), R_EAX);
}

TEST(target, x86_64_xmm_return_register)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->return_reg(t, 4, RC_XMM), R_XMM0);
}

TEST(target, x86_64_x87_return_register)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_EQ(t->return_reg(t, 16, RC_X87), R_X87_ST0);
}

TEST(target, x86_64_binop_needs_no_fixed_register)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_FALSE(t->needs_reg(t, OP_ADD, 8, false, 0));
    EXPECT_FALSE(t->needs_reg(t, OP_ADD, 8, false, 1));
}

TEST(target, x86_64_shift_needs_rcx)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_TRUE(t->needs_reg(t, OP_SHL, 8, false, 1)); /* count in %cl */
    EXPECT_FALSE(t->needs_reg(t, OP_SHL, 8, false, 0));
}

TEST(target, x86_64_division_needs_rax)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_TRUE(t->needs_reg(t, OP_SDIV, 8, false, 0)); /* dividend in %rax */
    EXPECT_FALSE(t->needs_reg(t, OP_SDIV, 8, false, 1));
}

TEST(target, x86_64_call_needs_abi_registers)
{
    const TargetDesc *t = x86_64_target();
    EXPECT_TRUE(t->needs_reg(t, OP_CALL, 4, false, 0)); /* args ride ABI regs */
}
