#ifndef FICC_IR_H
#define FICC_IR_H

#include "type.h"
#include "util/types.h"
#include "util/vec.h"

/* X-macro for IR opcodes. Append only.
   ICMP predicates are separate opcodes so IrInstr needs no predicate field. */
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
    X(OP_UNREACHABLE)

typedef enum
{
#define ENUM_ENTRY(K) K,
    IR_OPCODES(ENUM_ENTRY)
#undef ENUM_ENTRY
} IrOpcode;

#define NO_VREG 0xFFFFFFFFU

/* IrOperand: immediate, virtual register, or global reference.
   The union is named `u` so every arm is explicit at every use site. */
typedef struct
{
    bool is_imm;
    bool is_global;
    union
    {
        u32 vreg;
        i64 imm;
        u32 global_index;
    } u;
} IrOperand;

/* One entry per predecessor block. */
typedef struct
{
    IrOperand val;
    const char *label;
} IrPhiEntry;

/* SWITCH case: value → target block label. */
typedef struct
{
    i64 val;
    const char *label;
} IrSwitchCase;

/* Extended payloads for variable-arity instructions.
   Only the field matching the opcode is valid. */
typedef struct
{
    u32 nentries;
    u32 nfilled; /* tracks ir_phi_add_entry calls; verified by test_ir_builder */
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

/* IR instruction.
   Fixed operands in ops[3] cover unary/binary/ternary needs.
   Variable-arity ops (phi, switch, call) use the typed `extra` union.
   Steering-locked shape: do not change without design discussion. */
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
    bool is_loop_header; /* block is a loop header (back edge added later) */
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
    Vec *params;    /* Vec<IrParam*> */
    Vec *blocks;    /* Vec<IrBlock*> */
    bool is_static; /* internal linkage (stays local in the object file) */
};

/* IrGlobal variable / data record.
   init_data == NULL and init_len == 0 → .bss */
typedef enum
{
    IR_SECTION_DATA,
    IR_SECTION_RODATA,
    IR_SECTION_BSS,
} IrSection;

/* ELF-ish symbol linkage. Strings and static vars are local; default file-scope
   vars are global; extern vars are undefined, resolved at link. */
typedef enum
{
    IR_LINK_LOCAL,
    IR_LINK_GLOBAL,
    IR_LINK_EXTERN,
} IrLinkage;

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
    int init_reloc_target; /* global index whose address is written into the
                              init bytes (.rela.data R_X86_64_64), or -1 */
};

/* An IrModule owns all IR data for a compilation unit.
   Vreg ids are dense and module-wide; the width table is indexed by vreg. */
typedef struct IrModule IrModule;
struct IrModule
{
    Arena *arena;
    Vec *funcs;   /* Vec<IrFunction*> */
    Vec *globals; /* Vec<IrGlobal*> */
    u8 *widths;   /* value-width table, indexed by vreg id */
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
u32 ir_alloc_vreg(IrModule *m, u8 width);

/* instruction creation */
IrInstr *ir_emit_ret(IrBlock *bb, IrOperand val);
IrInstr *ir_emit_unreachable(IrBlock *bb);
IrInstr *ir_emit_ret_void(IrBlock *bb);

/* Binary (arith, shifts, icmp predicates) and unary ops: the opcode carries
   the operation, so adding an opcode needs no new emitter. */
IrInstr *ir_emit_binop(IrBlock *bb, IrOpcode op, u32 dst, IrOperand lhs, IrOperand rhs);
IrInstr *ir_emit_unary(IrBlock *bb, IrOpcode op, u32 dst, IrOperand src);
IrInstr *ir_emit_call(IrBlock *bb, u32 dst, const char *name, u32 nargs, IrOperand *args);
IrInstr *ir_emit_br(IrBlock *bb, const char *target_label);
IrInstr *ir_emit_brcond(IrBlock *bb, IrOperand cond, const char *true_label,
                        const char *false_label);
IrInstr *ir_emit_phi(IrBlock *bb, u32 dst, u32 nentries);
void ir_phi_add_entry(IrInstr *phi, IrOperand val, IrBlock *pred);

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

/* dump */
void ir_dump(IrModule *m);
const char *ir_opcode_name(IrOpcode op);

#endif
