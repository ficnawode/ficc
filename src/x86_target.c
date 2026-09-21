#include "target.h"
#include "x86_emit.h"

/* First six GP argument registers, in passing order (AMD64 psABI §3.2.3). */
static const u8 gp_args[6] = {R_EDI, R_ESI, R_EDX, R_ECX, R_R8, R_R9};

static const u8 fp_args[8] = {R_XMM0, R_XMM1, 2, 3, 4, 5, 6, 7};

/* Caller-saved GP registers a call may clobber. */
static const u8 clobbered_call[9] = {R_EAX, R_ECX, R_EDX, R_ESI, R_EDI, R_R8, R_R9, R_R10, R_R11};

static u8 x86_return_reg(const TargetDesc *t, u8 width, RegClass cls)
{
    (void) t;
    (void) width;
    if (cls == RC_X87)
    {
        return R_X87_ST0;
    }
    if (cls == RC_XMM)
    {
        return R_XMM0;
    }
    return R_EAX;
}

static bool x86_needs_reg(const TargetDesc *t, IrOpcode op, u8 width, bool is_fp, int opnd)
{
    (void) t;
    (void) width;
    (void) is_fp;
    switch (op)
    {
        case OP_SHL:
        case OP_LSHR:
        case OP_ASHR:
            return opnd == 1; /* x86 shifts take their count in %cl (fixed register). */
        case OP_SDIV:
        case OP_UDIV:
        case OP_SREM:
        case OP_UREM:
            return opnd == 0; /* x86 division tears the dividend apart in %rax. */
        case OP_CALL:
            return true; /* every call argument rides an ABI register or the stack. */
        default:
            return false;
    }
}

static const TargetDesc x86_64_desc = {
    .name = "x86-64",
    .gpr = {.cls = RC_GPR,
            .num_regs = 16,
            .names = {R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI, R_R8, R_R9, R_R10,
                      R_R11, R_R12, R_R13, R_R14, R_R15},
            .callee_saved = {R_EBX, R_EBP, R_R12, R_R13, R_R14, R_R15},
            .ncallee_saved = 6,
            .align = 8,
            .memory_only = false,
            .fixed = {R_ESP, R_EBP, R_EAX, R_ECX, R_EDX, R_ESI, R_EDI, R_R8, R_R9, R_R10, R_R11},
            .nfixed = 11},
    .xmm = {.cls = RC_XMM,
            .num_regs = 8,
            .names = {R_XMM0, R_XMM1, 2, 3, 4, 5, 6, 7},
            .ncallee_saved = 0,
            .align = 8,
            .memory_only = false,
            .fixed = {R_XMM0, R_XMM1, 2, 3, 4, 5, 6, 7},
            .nfixed = 8},
    .x87 = {.cls = RC_X87, .memory_only = true, .align = 16},
    .word_width = 8,
    .frame_align = 16,
    .spill_align = {[1] = 1, [2] = 2, [4] = 4, [8] = 8, [16] = 16},
    .gp_args = gp_args,
    .fp_args = fp_args,
    .ngp = 6,
    .nfp = 8,
    .return_reg = x86_return_reg,
    .clobbered_call = clobbered_call,
    .nclobbered_call = 9,
    .needs_reg = x86_needs_reg,
};

const TargetDesc *x86_64_target(void)
{
    return &x86_64_desc;
}
