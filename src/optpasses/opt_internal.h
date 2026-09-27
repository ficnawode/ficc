#ifndef FICC_OPT_INTERNAL_H
#define FICC_OPT_INTERNAL_H

/* Private optimizer surface shared by opt.c, its passes, and the opt tests. */
#include "cli.h"
#include "ir.h"
#include "util/arena.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "util/vec.h"
#include <stdbool.h>

bool opt_verify(struct IrModule *mod);

/* Variadic module error helper (the optimizer's own, like each pass module's). */
void opt_error(const char *fmt, ...);

/* CFG base: successors (from terminators) plus reverse postorder from entry. */
typedef struct CfgInfo
{
    struct IrFunction *func;
    u32 nblocks;
    u32 nreach; /* blocks reachable from the entry */
    Vec **succs;
    struct IrBlock **rpo;
    u32 *rpo_index; /* block index -> rpo[] position, UINT32_MAX when unreachable */
} CfgInfo;

CfgInfo *opt_cfg_build(struct IrFunction *f, Arena *arena);

typedef struct Dominators
{
    u32 nblocks;
    u32 *idom;  /* immediate dominator per block index (entry = itself) */
    u32 *depth; /* dominator-tree depth */
} Dominators;

Dominators *opt_doms_build(CfgInfo *cfg, Arena *arena);
bool opt_doms_dominates(const Dominators *doms, u32 a_idx, u32 b_idx);

typedef struct Loop
{
    struct IrBlock *header;
    Vec *blocks;               /* header + body blocks */
    Vec *latches;              /* back-edge sources into the header */
    struct IrBlock *preheader; /* single non-latch pred after canonicalization */
    struct Loop *outer;
    u32 index;
} Loop;

typedef struct
{
    Vec *loops;
} LoopInfo;

LoopInfo *opt_loops_find(struct IrFunction *f, CfgInfo *cfg, Dominators *doms, Arena *arena);
bool opt_loops_contains(const Loop *loop, struct IrBlock *bb);

/* At most one latch and one non-latch pred per loop, via synthetic blocks. */
bool opt_loops_canonicalize(struct IrModule *mod, struct IrFunction *f, LoopInfo *loops);
bool opt_loops_verify_shapes(LoopInfo *loops);

u32 opt_block_index(struct IrFunction *f, struct IrBlock *bb);
struct IrBlock *opt_block_by_label(struct IrFunction *f, const char *label);
/* label -> block map for the hot label-resolution paths. */
struct HashMap *opt_label_map_build(struct IrFunction *f, Arena *arena);

/* Optimizer context and shared magic (opt.c). */

typedef struct OptimizerContext OptimizerContext;

/* The optimizer's pass vocabulary; a level selects a sublist (reserved slots skip). */
typedef enum
{
    OPT_PASS_FOLD_CONST = 1,
    OPT_PASS_IDENTITY,
    OPT_PASS_CAST,
    OPT_PASS_CPROP,
    OPT_PASS_PHI_SIMP,
    OPT_PASS_DCE,
    OPT_PASS_CFG_CLEAN,
    OPT_PASS_PREHEADER,
    OPT_PASS_INLINE,
    OPT_PASS_DFE,
    OPT_PASS_GVN,
    OPT_PASS_LICM,
    OPT_PASS_MEM_FWD,
    OPT_PASS_REASSOC,
    OPT_PASS_STRENGTH,
    OPT_PASS_DSE,
    OPT_PASS_CANON,
} OptPassId;

/* A pass selection: the list and its length together. */
typedef struct
{
    const OptPassId *passes;
    u32 count;
} OptPassList;

/* The exact passes to run plus the fixpoint iteration budget they get. */
typedef struct
{
    OptPassList passlist;
    u32 max_iterations;
} OptConfig;

/* The concrete config each -O level expands to (static; level not stored). */
const OptConfig *opt_config_for(OptLevel level);

/* Registry row: id (config lookup), name (diagnostics), fn (NULL = reserved). */
typedef bool (*OptPassFn)(OptimizerContext *ctx);
typedef struct OptPass
{
    OptPassId id;
    const char *name;
    OptPassFn fn;
} OptPass;

struct OptimizerContext
{
    struct IrModule *mod;
    Arena *arena;
    Arena *scratch; /* per-pass analysis scratch; reset before each pass */
    bool changed;               /* a pass in the last table iteration changed the IR */
    const OptConfig *opts;      /* the pass selection driving the optimizer */
    struct IrFunction *cache_f; /* function the caches below describe (NULL: none) */
    u32 cfg_epoch;              /* bumped whenever a pass mutates the CFG */
    u32 cache_epoch;            /* cfg_epoch the caches were built at */
    CfgInfo *cfg;
    Dominators *doms;
    LoopInfo *loops;
    struct IrInstr **def_vreg; /* producing instruction per vreg */
    u32 *use_count;            /* operand-reference count per vreg */
    struct IrBlock **def_block;
    struct IrInstr **def_instr;
    u32 value_cap;
    u64 inline_sites;                   /* monotonic inline-site counter (unique clone labels) */
    struct HashMap *inline_lineage;     /* IrBlock* -> Vec<IrFunction*>: a clone's ancestry */
    struct HashMap *inline_caller_used; /* IrFunction* -> u32: clones a caller absorbed overall */
    IrOperand *repl_val;                /* per-vreg pending replacement operand */
    u32 *repl_gen;                      /* repl_val[l] is live when repl_gen[l] == repl_serial */
    u32 repl_serial;                    /* current replacement generation */
    u32 repl_cap;                       /* capacity of repl_val/repl_gen */
};

/* Cached per-function analysis, rebuilt when the function or cfg_epoch changes. */
CfgInfo *opt_get_cfg(OptimizerContext *ctx, struct IrFunction *f);
Dominators *opt_get_doms(OptimizerContext *ctx, struct IrFunction *f);
LoopInfo *opt_get_loops(OptimizerContext *ctx, struct IrFunction *f);

/* Blocks of f reachable from the entry, dominator-first reverse postorder. */
Vec *opt_rpo_order(OptimizerContext *ctx, struct IrFunction *f);

/* Instruction algebra (opt.c). */

u32 opt_instr_index(struct IrBlock *bb, struct IrInstr *in);
void opt_erase_instr(struct IrBlock *bb, struct IrInstr *in); /* vregs are never reused */
void opt_insert_instr(struct IrBlock *bb, u32 idx, struct IrInstr *in);
void opt_replace_operand(struct IrInstr *in, u8 which, IrOperand val);
void opt_copy_line(struct IrInstr *in,
                   const struct IrInstr *model); /* substitutions inherit line */

/* Edge splicing (opt.c). */

/* Split the edges in `preds` -> succ through one fresh empty block; returns it. */
struct IrBlock *opt_insert_empty_block(struct IrModule *mod, struct IrFunction *f, Vec *preds,
                                       struct IrBlock *succ, const char *prefix);
/* Single-predecessor form of opt_insert_empty_block (the preheader shape). */
struct IrBlock *opt_insert_preheader(struct IrModule *mod, struct IrFunction *f,
                                     struct IrBlock *pred, struct IrBlock *succ,
                                     const char *prefix);
/* Retarget `from`'s terminator edges naming `old_label` to `new_label`. */
void opt_retarget_terminator(struct IrBlock *from, const char *old_label, const char *new_label);
/* Remove the from->to edge: drop `from` from to->preds and from to's phis. */
void opt_drop_edge(struct IrBlock *from, struct IrBlock *to);

/* Value analysis (opt.c). */

void opt_ensure_value_arrays(OptimizerContext *ctx);
void opt_make_value_analysis(OptimizerContext *ctx, struct IrFunction *f);

/* Def replacement (opt.c): schedule vreg -> operand substitutions, then resolve. */
void opt_repl_begin(OptimizerContext *ctx);
void opt_repl_set(OptimizerContext *ctx, u32 vreg, IrOperand val);
bool opt_repl_apply(OptimizerContext *ctx, struct IrFunction *f);

/* Scalar value semantics (opt_value.c). */

bool opt_operand_eq(IrOperand a, IrOperand b);

/* Mask/sign-extend `raw` to a value's width class, as the interpreter does. */
i64 opt_normalize(i64 raw, u8 width_bytes, bool is_signed);

/* Fold an all-imm integer op to normalized bits; false on UB (zero div, bad shift). */
bool opt_fold_int(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

/* Fold an all-imm FP op re-rounded at the result width; false for width 16. */
bool opt_fold_fp(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

/* Fold an all-imm FP compare to its 0/1 integer result. */
bool opt_fold_fcmp(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

/* Canonicalize passes (one file each). */

bool opt_pass_fold_const(OptimizerContext *ctx);
bool opt_pass_identity(OptimizerContext *ctx);
bool opt_pass_cast(OptimizerContext *ctx);
bool opt_pass_cprop(OptimizerContext *ctx);
bool opt_pass_phi_simp(OptimizerContext *ctx);
bool opt_pass_dce(OptimizerContext *ctx);
bool opt_pass_cfg_clean(OptimizerContext *ctx);
bool opt_pass_preheader(OptimizerContext *ctx);

/* Optimize passes (one file each). */

bool opt_pass_inline(OptimizerContext *ctx);
bool opt_pass_dfe(OptimizerContext *ctx);
bool opt_pass_gvn(OptimizerContext *ctx);
bool opt_pass_licm(OptimizerContext *ctx);
bool opt_pass_mem_fwd(OptimizerContext *ctx);
bool opt_pass_strength(OptimizerContext *ctx);
bool opt_pass_reassoc(OptimizerContext *ctx);
bool opt_pass_dse(OptimizerContext *ctx);
bool opt_pass_canon(OptimizerContext *ctx);

#endif
