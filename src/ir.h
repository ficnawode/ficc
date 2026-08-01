#ifndef FICC_IR_H
#define FICC_IR_H

#include "type.h"
#include "util/types.h"
#include "util/vec.h"

/* X-macro for IR opcodes. Append only.
   ICMP predicates are separate opcodes so Instr needs no predicate field. */
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
    X(OP_BR)                                                                                       \
    X(OP_BRCOND)                                                                                   \
    X(OP_SWITCH)                                                                                   \
    X(OP_CALL)                                                                                     \
    X(OP_PHI)                                                                                      \
    X(OP_SELECT)                                                                                   \
    X(OP_UNREACHABLE)

typedef enum {
#define ENUM_ENTRY(K) K,
    IR_OPCODES(ENUM_ENTRY)
#undef ENUM_ENTRY
} IrOpcode;

#define NO_VREG 0xFFFFFFFFU

/* Operand: either an immediate or a virtual register index.
   The union is named `u` so both arms are explicit at every use site. */
typedef struct {
    bool is_imm;
    union {
        u32 vreg;
        i64 imm;
    } u;
} Operand;

/* Entries for PHI nodes: one per predecessor block. */
typedef struct {
    u32 vreg;
    const char *label;
} PhiEntry;

/* Cases for SWITCH: value → target block label. */
typedef struct {
    i64 val;
    const char *label;
} SwitchCase;

/* Extended payloads for variable-arity instructions.
   Only the field matching the opcode is valid. */
typedef struct {
    u32 nentries;
    PhiEntry *entries;
} PhiPayload;

typedef struct {
    u32 ncases;
    SwitchCase *cases;
    const char *default_label;
} SwitchPayload;

typedef struct {
    u32 nargs;
    Operand *args;
    const char *name;
} CallPayload;

/* IR instruction.
   Fixed operands in ops[3] cover unary/binary/ternary needs.
   Variable-arity ops (phi, switch, call) use the typed `extra` union.
   Steering-locked shape: do not change without design discussion. */
typedef struct Instr Instr;
struct Instr {
    IrOpcode opcode;
    u32 result;      /* vreg index, or NO_VREG */
    u8 nops;
    Operand ops[3];
    union {
        PhiPayload phi;
        SwitchPayload sw;
        CallPayload call;
    } extra;
};

typedef struct Block Block;
struct Block {
    const char *label;
    Vec *instrs; /* Vec<Instr*> */
};

/* Parameter descriptor for a Function. */
typedef struct Param Param;
struct Param {
    const char *name;
    Type *type;
    u32 vreg;
};

typedef struct Function Function;
struct Function {
    const char *name;
    Type *ret_type;
    Vec *params;    /* Vec<Param*> */
    Vec *blocks;    /* Vec<Block*> */
};

/* Global variable / data record.
   init_data == NULL and init_len == 0 → .bss */
typedef struct Global Global;
struct Global {
    const char *name;
    Type *type;
    const u8 *init_data;
    size_t init_len;
    u32 align;
};

/* Module owns all IR data for a compilation unit.
   Vreg ids are dense and module-wide; the width table is indexed by vreg. */
typedef struct Module Module;
struct Module {
    Arena *arena;
    Vec *funcs;        /* Vec<Function*> */
    Vec *globals;      /* Vec<Global*> */
    u8 *widths;        /* value-width table, indexed by vreg id */
    u32 width_count;
    u32 width_cap;
    u32 next_vreg;     /* module-wide vreg allocator */
};

/* ---- builder ---- */
Module *ir_module_new(Arena *arena);
Function *ir_module_add_func(Module *m, Arena *arena, const char *name, Type *ret_type);
Block *ir_func_add_block(Function *f, Arena *arena, const char *label);

/* vreg allocation */
u32 ir_alloc_vreg(Module *m, u8 width);

/* instruction creation */
Instr *ir_emit_ret(Block *b, Arena *arena, Operand val);
Instr *ir_emit_unreachable(Block *b, Arena *arena);
Instr *ir_emit_ret_void(Block *b, Arena *arena);
Instr *ir_emit_add(Block *b, Arena *arena, u32 dst, Operand lhs, Operand rhs);
Instr *ir_emit_sub(Block *b, Arena *arena, u32 dst, Operand lhs, Operand rhs);
Instr *ir_emit_mul(Block *b, Arena *arena, u32 dst, Operand lhs, Operand rhs);
Instr *ir_emit_sdiv(Block *b, Arena *arena, u32 dst, Operand lhs, Operand rhs);
Instr *ir_emit_srem(Block *b, Arena *arena, u32 dst, Operand lhs, Operand rhs);
Instr *ir_emit_neg(Block *b, Arena *arena, u32 dst, Operand src);
Instr *ir_emit_call(Block *b, Arena *arena, u32 dst, const char *name, u32 nargs, Operand *args);

/* operand helpers */
Operand ir_operand_imm(i64 val);
Operand ir_operand_vreg(u32 vreg);

/* dump */
void ir_dump(Module *m);
const char *ir_opcode_name(IrOpcode op);

#endif
