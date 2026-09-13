#ifndef FICC_IR_H
#define FICC_IR_H

#include "type.h"
#include "util/types.h"
#include "util/vec.h"

/* IR opcode X-macro; append only. ICMP predicates are separate opcodes for a flat IrInstr. */
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

/* Immediate, vreg, global, or function-address value; the is_* flag selects the `u` arm. */
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
        const char *func_name; /* function designator's name, valid when is_func */
    } u;
} IrOperand;

/* One phi entry, holding the value arriving from a named predecessor block. */
typedef struct
{
    IrOperand val;
    const char *label;
} IrPhiEntry;

/* A SWITCH case: a case value mapping to a target block label. */
typedef struct
{
    i64 val;
    const char *label;
} IrSwitchCase;

/* Payloads for variable-arity instructions; only the `extra` arm matching the opcode is valid. */
typedef struct
{
    u32 nentries;
    u32 nfilled; /* ir_phi_add_entry call count, verified by test_ir_builder */
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
    bool is_variadic; /* the callee is variadic; the call site zeroes %al */
    bool is_indirect; /* the callee is an operand value, not a named symbol */
    IrOperand callee; /* the indirect-call target, when is_indirect */
} IrCallPayload;

typedef struct
{
    const char *true_label;
    const char *false_label;
} IrBrcondPayload;

typedef struct
{
    const char *target_label;
} IrBrPayload;

/* Fixed operands in ops[3] cover unary/binary/ternary ops; variable-arity ops use `extra`. */
typedef struct IrInstr IrInstr;
struct IrInstr
{
    IrOpcode opcode;
    u32 result; /* vreg index, or NO_VREG */
    u8 nops;
    IrOperand ops[3];
    union
    {
        IrPhiPayload phi;
        IrSwitchPayload sw;
        IrCallPayload call;
        IrBrPayload br;
        IrBrcondPayload brcond;
    } extra;
};

typedef struct IrBlock IrBlock;
struct IrBlock
{
    const char *label;
    Arena *arena;
    Vec *instrs;         /* Vec<IrInstr*> */
    Vec *preds;          /* Vec<IrBlock*> — predecessor blocks */
    bool sealed;         /* all predecessors known? */
    bool is_loop_header; /* block is a loop header (back edges added later) */
};

typedef struct IrParam IrParam;
struct IrParam
{
    const char *name;
    Type *type;
    u32 vreg;
};

typedef struct IrFunction IrFunction;
struct IrFunction
{
    const char *name;
    Type *ret_type;
    Arena *arena;
    Vec *params;      /* Vec<IrParam*> */
    Vec *blocks;      /* Vec<IrBlock*> */
    bool is_static;   /* internal linkage (stays local in the object file) */
    bool is_variadic; /* trailing unnamed args beyond the named params (C11 §6.7.6.3p8) */
};

/* init_data == NULL && init_len == 0 → .bss */
typedef enum
{
    IR_SECTION_DATA,
    IR_SECTION_RODATA,
    IR_SECTION_BSS,
} IrSection;

/* ELF-ish linkage: strings/statics are local, file-scope vars global, extern vars undefined. */
typedef enum
{
    IR_LINK_LOCAL,
    IR_LINK_GLOBAL,
    IR_LINK_EXTERN,
} IrLinkage;

/* A pointer-typed 8-byte slot in a global's init_data, relocated to a symbol's address. */
typedef struct GlobalReloc GlobalReloc;
struct GlobalReloc
{
    u32 offset;            /* byte offset into init_data */
    int target;            /* global index whose address is written here; -1 when is_func */
    bool is_func;          /* the address written is a function's */
    const char *func_name; /* the referenced function, when is_func */
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
    Vec *relocs; /* Vec<GlobalReloc*>, NULL when there are no address constants to relocate */
};

/* Owns all IR for one compilation unit; vreg ids are dense and module-wide. */
typedef struct IrModule IrModule;
struct IrModule
{
    Arena *arena;
    Vec *funcs;       /* Vec<IrFunction*> */
    Vec *globals;     /* Vec<IrGlobal*> */
    u8 *widths;       /* value-width table, indexed by vreg id */
    bool *signedness; /* signedness table, indexed by vreg id (parallel to widths) */
    bool *floatness;  /* FP-class table, indexed by vreg id (parallel to widths) */
    u32 width_count;
    u32 width_cap;
    u32 next_vreg; /* module-wide vreg allocator */
};

/* ---- builder ----
   Every builder allocates from the module's arena, reached through the
   module/function/block context. ir_module_new is the sole entry point. */
IrModule *ir_module_new(Arena *arena);
IrFunction *ir_module_add_func(IrModule *m, const char *name, Type *ret_type);
IrBlock *ir_func_add_block(IrFunction *f, const char *label);

/* vreg allocation */
u32 ir_alloc_vreg(IrModule *m, u8 width, bool is_signed, bool is_float);
u32 ir_alloc_fp_vreg(IrModule *m, u8 width);
bool ir_vreg_signed(const IrModule *m, u32 vreg);
bool ir_vreg_float(const IrModule *m, u32 vreg);

/* instruction creation */
IrInstr *ir_emit_ret(IrBlock *bb, IrOperand val);
IrInstr *ir_emit_unreachable(IrBlock *bb);
IrInstr *ir_emit_ret_void(IrBlock *bb);

/* The opcode carries the operation, so adding one needs no new emitter. */
IrInstr *ir_emit_binop(IrBlock *bb, IrOpcode op, u32 dst, IrOperand lhs, IrOperand rhs);
IrInstr *ir_emit_unary(IrBlock *bb, IrOpcode op, u32 dst, IrOperand src);
IrInstr *ir_emit_call(IrBlock *bb, u32 dst, const char *name, u32 nargs, IrOperand *args);
void ir_call_set_variadic(IrInstr *call, bool is_variadic);
void ir_call_set_indirect(IrInstr *call, IrOperand callee);
IrInstr *ir_emit_va_start(IrBlock *bb, IrOperand ap, i64 stack_skip, i64 gp_offset, i64 fp_offset);
IrInstr *ir_emit_va_arg(IrBlock *bb, u32 dst, IrOperand ap);
IrInstr *ir_emit_va_end(IrBlock *bb, IrOperand ap);
IrInstr *ir_emit_br(IrBlock *bb, const char *target_label);
IrInstr *ir_emit_brcond(IrBlock *bb, IrOperand cond, const char *true_label,
                        const char *false_label);
IrInstr *ir_emit_phi(IrBlock *bb, u32 dst, u32 nentries);
void ir_phi_add_entry(IrInstr *phi, IrOperand val, IrBlock *pred);
IrInstr *ir_emit_switch(IrBlock *bb, IrOperand val, u32 ncases, IrSwitchCase *cases,
                        const char *default_label);

/* memory ops */
IrInstr *ir_emit_load(IrBlock *bb, u32 dst, IrOperand ptr);
IrInstr *ir_emit_store(IrBlock *bb, IrOperand val, IrOperand ptr, u32 width_bytes);
IrInstr *ir_emit_gep(IrBlock *bb, u32 dst, IrOperand base, IrOperand index, u32 stride);
IrInstr *ir_emit_alloca(IrBlock *bb, u32 dst, u32 size_bytes);
IrInstr *ir_emit_memcpy(IrBlock *bb, IrOperand dst, IrOperand src, u32 size_bytes);

/* operand helpers */
IrOperand ir_operand_imm(i64 val);
IrOperand ir_operand_vreg(u32 vreg);
IrOperand ir_operand_global(u32 global_index);
IrOperand ir_operand_func(const char *func_name);

/* dump */
void ir_dump(IrModule *m);
const char *ir_opcode_name(IrOpcode op);

#endif
