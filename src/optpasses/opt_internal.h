#ifndef FICC_OPT_INTERNAL_H
#define FICC_OPT_INTERNAL_H

/* Private opt surface for the passes and the unit tests. */
#include "util/types.h"
#include "util/vec.h"
#include <stdbool.h>

struct IrModule;
struct IrFunction;
struct IrBlock;

bool opt_verify(struct IrModule *mod);

/* CFG base: successors derived from terminators (real edges), plus an
   iterative reverse postorder from the entry block. */
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

/* Canonical shape: at most one latch (merged through a synthetic block with
   value-merging PHIs) and at most one non-latch pred (spliced through a
   synthetic preheader). */
bool opt_loops_canonicalize(struct IrModule *mod, struct IrFunction *f, LoopInfo *loops);
bool opt_loops_verify_shapes(LoopInfo *loops);

u32 opt_block_index(struct IrFunction *f, struct IrBlock *bb);
struct IrBlock *opt_block_by_label(struct IrFunction *f, const char *label);

#endif