#ifndef FICC_IR_H
#define FICC_IR_H

#include "type.h"
#include "util/types.h"
#include "util/vec.h"

#define IR_OPCODES(X)                                                                              \
    X(OP_RET)                                                                                      \
    X(OP_ADD)                                                                                      \
    X(OP_SUB)                                                                                      \
    X(OP_MUL)                                                                                      \
    X(OP_SDIV)                                                                                     \
    X(OP_UDIV)                                                                                     \
    X(OP_SREM)                                                                                     \
    X(OP_UREM)                                                                                     \
    X(OP_AND)                                                                                      \
    X(OP_OR)                                                                                       \
    X(OP_XOR)                                                                                      \
    X(OP_SHL)                                                                                      \
    X(OP_LSHR)                                                                                     \
    X(OP_ASHR)                                                                                     \
    X(OP_NEG)                                                                                      \
    X(OP_NOT)                                                                                      \
    X(OP_ICMP_EQ)                                                                                  \
    X(OP_ICMP_NE)                                                                                  \
    X(OP_ICMP_ULT)                                                                                 \
    X(OP_ICMP_ULE)                                                                                 \
    X(OP_ICMP_UGT)                                                                                 \
    X(OP_ICMP_UGE)                                                                                 \
    X(OP_ICMP_SLT)                                                                                 \
    X(OP_ICMP_SLE)                                                                                 \
    X(OP_ICMP_SGT)                                                                                 \
    X(OP_ICMP_SGE)                                                                                 \
    X(OP_TRUNC)                                                                                    \
    X(OP_ZEXT)                                                                                     \
    X(OP_SEXT)                                                                                     \
    X(OP_LOAD)                                                                                     \
    X(OP_STORE)                                                                                    \
    X(OP_GEP)                                                                                      \
    X(OP_ALLOCA)                                                                                   \
    X(OP_MEMCPY)                                                                                   \
    X(OP_BR)                                                                                       \
    X(OP_BRCOND)                                                                                   \
    X(OP_SWITCH)                                                                                   \
    X(OP_CALL)                                                                                     \
    X(OP_PHI)                                                                                      \
    X(OP_SELECT)                                                                                   \
    X(OP_UNREACHABLE)                                                                              \
    X(OP_VA_START)                                                                                 \
    X(OP_VA_ARG)                                                                                   \
    X(OP_VA_END)                                                                                   \
    X(OP_ITOF)                                                                                     \
    X(OP_FTOI)                                                                                     \
    X(OP_FCONV)                                                                                    \
    X(OP_FADD)                                                                                     \
    X(OP_FSUB)                                                                                     \
    X(OP_FMUL)                                                                                     \
    X(OP_FDIV)                                                                                     \
    X(OP_FNEG)                                                                                     \
    X(OP_FCMP_EQ)                                                                                  \
    X(OP_FCMP_NE)                                                                                  \
    X(OP_FCMP_LT)                                                                                  \
    X(OP_FCMP_GT)                                                                                  \
    X(OP_FCMP_LE)                                                                                  \
    X(OP_FCMP_GE)

typedef enum
{
#define ENUM_ENTRY(K) K,
    IR_OPCODES(ENUM_ENTRY)
#undef ENUM_ENTRY
} IrOpcode;

#define NO_VREG 0xFFFFFFFFU

typedef struct
{
    bool is_imm;
    bool is_global;
    bool is_func;
    union
    {
        u32 vreg;
        i64 imm;
        u32 global_index;
        const char *func_name;
    } u;
} IrOperand;

typedef struct
{
    IrOperand val;
    const char *label;
} IrPhiEntry;

typedef struct
{
    i64 val;
    const char *label;
} IrSwitchCase;

typedef struct
{
    u32 nentries;
    u32 nfilled;
    IrPhiEntry *entries;
} IrPhiPayload;

typedef struct
{
    u32 ncases;
    IrSwitchCase *cases;
    const char *default_label;
} IrSwitchPayload;

typedef struct
{
    u32 nargs;
    IrOperand *args;
    const char *name;
    bool is_variadic;
    bool is_indirect;
    IrOperand callee;
    Type **arg_types;
    Type *ret_type;
} IrCallPayload;

typedef struct
{
    const char *true_label;
    const char *false_label;
} IrBrcondPayload;

typedef struct
{
    i64 stack_skip;
    i64 gp_offset;
    i64 fp_offset;
} IrVaStartPayload;

typedef struct
{
    const char *target_label;
} IrBrPayload;

/* OP_LOAD/OP_STORE: the optimizer may not move, CSE, or forward volatile accesses. */
typedef struct
{
    bool is_volatile;
} IrMemPayload;

typedef struct IrInstr IrInstr;
struct IrInstr
{
    IrOpcode opcode;
    u32 result;
    u32 line;
    u8 nops;
    IrOperand ops[3];
    union
    {
        IrPhiPayload phi;
        IrSwitchPayload sw;
        IrCallPayload call;
        IrBrPayload br;
        IrBrcondPayload brcond;
        IrVaStartPayload va_start;
        IrMemPayload mem;
    } extra;
    u32 frame_off;
};

typedef struct IrFunction IrFunction;

typedef struct IrBlock IrBlock;
struct IrBlock
{
    const char *label;
    IrFunction *func;
    Arena *arena;
    Vec *instrs; /* Vec<IrInstr*> */
    Vec *preds;  /* Vec<IrBlock*> */
    bool sealed;
    bool is_loop_header;
    u32 index;
};

typedef struct IrParam IrParam;
struct IrParam
{
    const char *name;
    Type *type;
    u32 vreg;
    Type *agg_type;
};

typedef struct IrLocal IrLocal;
struct IrLocal
{
    const char *name;
    Type *type;
    Vec *vregs;
};

struct IrFunction
{
    const char *name;
    Type *ret_type;
    Arena *arena;
    Vec *params; /* Vec<IrParam*> */
    Vec *blocks; /* Vec<IrBlock*> */
    Vec *locals; /* Vec<IrLocal*> */
    u32 cur_line;
    bool is_static;
    bool is_variadic; /* C11 §6.7.6.3p8 */
    bool is_inline;   /* C11 §6.7.4 */
};

/* init_data == NULL && init_len == 0 → .bss */
typedef enum
{
    IR_SECTION_DATA,
    IR_SECTION_RODATA,
    IR_SECTION_BSS,
    IR_SECTION_INIT_ARRAY,
    IR_SECTION_FINI_ARRAY,
} IrSection;

typedef enum
{
    IR_LINK_LOCAL,
    IR_LINK_GLOBAL,
    IR_LINK_EXTERN,
} IrLinkage;

typedef struct GlobalReloc GlobalReloc;
struct GlobalReloc
{
    u32 offset;
    int target;
    bool is_func;
    const char *func_name;
    i64 addend;
};

typedef struct IrGlobal IrGlobal;
struct IrGlobal
{
    const char *name;
    Type *type;
    const u8 *init_data;
    size_t init_len;
    u32 align;
    IrSection section;
    IrLinkage linkage;
    Vec *relocs; /* Vec<GlobalReloc*> */
};

typedef struct IrModule IrModule;
struct IrModule
{
    Arena *arena;
    Vec *funcs;   /* Vec<IrFunction*> */
    Vec *globals; /* Vec<IrGlobal*> */
    u8 *widths;
    bool *signedness;
    bool *floatness;
    u32 width_count;
    u32 width_cap;
    u32 next_vreg;
};

IrModule *ir_module_new(Arena *arena);
IrFunction *ir_module_add_func(IrModule *m, const char *name, Type *ret_type);
IrBlock *ir_func_add_block(IrFunction *f, const char *label);
IrLocal *ir_func_add_local(IrFunction *f, const char *name, Type *type);
void ir_func_remove_block(IrFunction *f, IrBlock *bb);

u32 ir_alloc_vreg(IrModule *m, u8 width, bool is_signed, bool is_float);
u32 ir_alloc_fp_vreg(IrModule *m, u8 width);
bool ir_vreg_signed(const IrModule *m, u32 vreg);
bool ir_vreg_float(const IrModule *m, u32 vreg);

IrInstr *ir_emit_ret(IrBlock *bb, IrOperand val);
IrInstr *ir_emit_unreachable(IrBlock *bb);
IrInstr *ir_emit_ret_void(IrBlock *bb);

IrInstr *ir_emit_binop(IrBlock *bb, IrOpcode op, u32 dst, IrOperand lhs, IrOperand rhs);
IrInstr *ir_emit_unary(IrBlock *bb, IrOpcode op, u32 dst, IrOperand src);
IrInstr *ir_emit_call(IrBlock *bb, u32 dst, const char *name, u32 nargs, IrOperand *args);
void ir_call_set_variadic(IrInstr *call, bool is_variadic);
void ir_call_set_indirect(IrInstr *call, IrOperand callee);
void ir_call_set_types(IrInstr *call, Type **arg_types, Type *ret_type);
IrInstr *ir_emit_va_start(IrBlock *bb, IrOperand ap, i64 stack_skip, i64 gp_offset, i64 fp_offset);
IrInstr *ir_emit_va_arg(IrBlock *bb, u32 dst, IrOperand ap);
IrInstr *ir_emit_va_end(IrBlock *bb, IrOperand ap);
IrInstr *ir_emit_br(IrBlock *bb, const char *target_label);
IrInstr *ir_emit_brcond(IrBlock *bb, IrOperand cond, const char *true_label,
                        const char *false_label);
IrInstr *ir_emit_phi(IrBlock *bb, u32 dst, u32 nentries);
IrInstr *ir_emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries);
void ir_phi_add_entry(IrInstr *phi, IrOperand val, IrBlock *pred);
IrInstr *ir_emit_switch(IrBlock *bb, IrOperand val, u32 ncases, IrSwitchCase *cases,
                        const char *default_label);

IrInstr *ir_emit_load(IrBlock *bb, u32 dst, IrOperand ptr, bool is_volatile);
IrInstr *ir_emit_store(IrBlock *bb, IrOperand val, IrOperand ptr, u32 width_bytes,
                       bool is_volatile);
IrInstr *ir_emit_gep(IrBlock *bb, u32 dst, IrOperand base, IrOperand index, u32 stride);
IrInstr *ir_emit_alloca(IrBlock *bb, u32 dst, u32 size_bytes);
IrInstr *ir_emit_memcpy(IrBlock *bb, IrOperand dst, IrOperand src, u32 size_bytes);

IrOperand ir_operand_imm(i64 val);
IrOperand ir_operand_vreg(u32 vreg);
IrOperand ir_operand_global(u32 global_index);
IrOperand ir_operand_func(const char *func_name);
bool ir_operand_is_vreg(IrOperand op);

void ir_dump(IrModule *m);
const char *ir_opcode_name(IrOpcode op);

#endif
