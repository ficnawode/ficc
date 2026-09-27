#ifndef FICC_OPT_INTERNAL_H
#define FICC_OPT_INTERNAL_H

#include "cli.h"
#include "ir.h"
#include "util/arena.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "util/vec.h"
#include <stdbool.h>

bool opt_verify(struct IrModule *mod);

void opt_error(const char *fmt, ...);

typedef struct CfgInfo
{
    struct IrFunction *func;
    u32 nblocks;
    u32 nreach;
    Vec **succs;
    struct IrBlock **rpo;
    u32 *rpo_index;
} CfgInfo;

CfgInfo *opt_cfg_build(struct IrFunction *f, Arena *arena);

typedef struct Dominators
{
    u32 nblocks;
    u32 *idom;
    u32 *depth;
} Dominators;

Dominators *opt_doms_build(CfgInfo *cfg, Arena *arena);
bool opt_doms_dominates(const Dominators *doms, u32 a_idx, u32 b_idx);

typedef struct Loop
{
    struct IrBlock *header;
    Vec *blocks;
    Vec *latches;
    struct IrBlock *preheader;
    struct Loop *outer;
    u32 index;
} Loop;

typedef struct
{
    Vec *loops;
} LoopInfo;

LoopInfo *opt_loops_find(struct IrFunction *f, CfgInfo *cfg, Dominators *doms, Arena *arena);
bool opt_loops_contains(const Loop *loop, struct IrBlock *bb);

bool opt_loops_canonicalize(struct IrModule *mod, struct IrFunction *f, LoopInfo *loops);
bool opt_loops_verify_shapes(LoopInfo *loops);

u32 opt_block_index(struct IrFunction *f, struct IrBlock *bb);
struct IrBlock *opt_block_by_label(struct IrFunction *f, const char *label);
struct HashMap *opt_label_map_build(struct IrFunction *f, Arena *arena);

typedef struct OptimizerContext OptimizerContext;

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

typedef struct
{
    const OptPassId *passes;
    u32 count;
} OptPassList;

typedef struct
{
    OptPassList passlist;
    u32 max_iterations;
} OptConfig;

const OptConfig *opt_config_for(OptLevel level);

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
    Arena *scratch;
    bool changed;
    const OptConfig *opts;
    struct IrFunction *cache_f;
    u32 cfg_epoch;
    u32 cache_epoch;
    CfgInfo *cfg;
    Dominators *doms;
    LoopInfo *loops;
    struct IrInstr **def_vreg;
    u32 *use_count;
    struct IrBlock **def_block;
    struct IrInstr **def_instr;
    u32 value_cap;
    u64 inline_sites;
    struct HashMap *inline_lineage;
    struct HashMap *inline_caller_used;
    IrOperand *repl_val;
    u32 *repl_gen;
    u32 repl_serial;
    u32 repl_cap;
};

CfgInfo *opt_get_cfg(OptimizerContext *ctx, struct IrFunction *f);
Dominators *opt_get_doms(OptimizerContext *ctx, struct IrFunction *f);
LoopInfo *opt_get_loops(OptimizerContext *ctx, struct IrFunction *f);

Vec *opt_rpo_order(OptimizerContext *ctx, struct IrFunction *f);

u32 opt_instr_index(struct IrBlock *bb, struct IrInstr *in);
void opt_erase_instr(struct IrBlock *bb, struct IrInstr *in);
void opt_insert_instr(struct IrBlock *bb, u32 idx, struct IrInstr *in);
void opt_replace_operand(struct IrInstr *in, u8 which, IrOperand val);
void opt_copy_line(struct IrInstr *in, const struct IrInstr *model);

struct IrBlock *opt_insert_empty_block(struct IrModule *mod, struct IrFunction *f, Vec *preds,
                                       struct IrBlock *succ, const char *prefix);
struct IrBlock *opt_insert_preheader(struct IrModule *mod, struct IrFunction *f,
                                     struct IrBlock *pred, struct IrBlock *succ,
                                     const char *prefix);
void opt_retarget_terminator(struct IrBlock *from, const char *old_label, const char *new_label);
void opt_drop_edge(struct IrBlock *from, struct IrBlock *to);

void opt_ensure_value_arrays(OptimizerContext *ctx);
void opt_make_value_analysis(OptimizerContext *ctx, struct IrFunction *f);

void opt_repl_begin(OptimizerContext *ctx);
void opt_repl_set(OptimizerContext *ctx, u32 vreg, IrOperand val);
bool opt_repl_apply(OptimizerContext *ctx, struct IrFunction *f);

bool opt_operand_eq(IrOperand a, IrOperand b);

i64 opt_normalize(i64 raw, u8 width_bytes, bool is_signed);

bool opt_fold_int(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

bool opt_fold_fp(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

bool opt_fold_fcmp(const struct IrModule *mod, const struct IrInstr *in, i64 *out);

bool opt_pass_fold_const(OptimizerContext *ctx);
bool opt_pass_identity(OptimizerContext *ctx);
bool opt_pass_cast(OptimizerContext *ctx);
bool opt_pass_cprop(OptimizerContext *ctx);
bool opt_pass_phi_simp(OptimizerContext *ctx);
bool opt_pass_dce(OptimizerContext *ctx);
bool opt_pass_cfg_clean(OptimizerContext *ctx);
bool opt_pass_preheader(OptimizerContext *ctx);

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
