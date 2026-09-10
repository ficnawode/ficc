#include "ir_builder.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct PendingPhi PendingPhi;
struct PendingPhi
{
    ASTVarDecl *var;
    u32 vreg;
};

typedef struct BlockLocals BlockLocals;
struct BlockLocals
{
    U64Map *locals;    /* (u64)ASTVarDecl* -> IrOperand* */
    Vec *pending_phis; /* Vec<PendingPhi*> */
};

typedef struct ExprResult ExprResult;
struct ExprResult
{
    IrOperand value;
    IrBlock *block;
};

/* A lowered write target: an SSA slot for block-scope scalars, or an explicit
   address operand for globals, statics, spilled autos, member GEPs, derefs,
   subscripts, and record/array storage pointers. */
typedef struct LvalueSlot LvalueSlot;
struct LvalueSlot
{
    ASTVarDecl *decl; /* SSA target (when is_ssa) */
    bool is_ssa;
    IrOperand addr; /* memory address (when !is_ssa) */
    Type *type;
};

typedef struct LvalueResult LvalueResult;
struct LvalueResult
{
    LvalueSlot slot;
    IrBlock *block;
    bool failed;
};

/* Shared arithmetic-lowering inputs; plain binaries and compound assignment
   differ only in converting the result back to the LHS type. */
typedef struct
{
    BinOpKind op;
    Type *lt;
    Type *rt;
    Type *result_type;
    ASTNode *node;
} ArithSpec;

typedef struct
{
    IrOperand value;
    IrBlock *block;
    Type *result_type;
} ArithResult;

typedef struct LoopBlocks LoopBlocks;
struct LoopBlocks
{
    IrBlock *header;
    IrBlock *body;
    IrBlock *latch;
    IrBlock *exit;
};

typedef struct LoopContext LoopContext;
struct LoopContext
{
    IrBlock *continue_target;
    IrBlock *exit;
};

typedef struct SwitchCtx SwitchCtx;
struct SwitchCtx
{
    U64Map *case_blocks; /* (u64)case value -> IrBlock* */
    IrBlock *default_bb; /* NULL if no `default:` label */
    Vec *default_stmts;  /* statements following `default:` (NULL if none) */
    IrBlock *exit_bb;
};

typedef struct SwitchCase SwitchCase;
struct SwitchCase
{
    i64 value;
    Vec *stmts; /* Vec<ASTNode*>: statements under this `case N:` label */
    IrBlock *bb;
};

typedef struct FuncBuilder FuncBuilder;
struct FuncBuilder
{
    IrModule *mod;
    U64Map *block_locals; /* (u64)IrBlock* -> BlockLocals* */
    Vec *loop_stack;      /* Vec<LoopContext*> */
    Vec *switch_stack;    /* Vec<SwitchCtx*> */
    StrMap *goto_labels;  /* label name -> IrBlock* */
    StrMap *func_types;   /* function name -> Type* (interned function type) */
    StrMap *global_map;   /* file-scope variable name -> u32* (index into mod->globals) */
    U64Map *static_map;   /* (u64)ASTVarDecl* -> u32* (index into mod->globals) */
    Vec *spilled;         /* Vec<ASTVarDecl*>: block-scope autos whose address is taken */
    U64Map *spill_slots;  /* (u64)ASTVarDecl* -> IrOperand* (function-entry slot addr) */
    u32 sret_vreg;        /* hidden sret pointer vreg for record-returning funcs */
    bool failed;          /* an error was reported while building this function */
};

static IrOperand resolve_variable(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb);
static u32 global_index_of(FuncBuilder *ctx, const char *name);
static u32 block_static_index(FuncBuilder *ctx, ASTVarDecl *var);
static u32 var_global_index(FuncBuilder *ctx, ASTVarDecl *decl);
static IrOperand build_phi(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb);
static IrInstr *emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries);
static void fill_phi_entries(FuncBuilder *ctx, IrBlock *bb, ASTVarDecl *var, IrInstr *phi);
static IrBlock *label_block(FuncBuilder *ctx, IrFunction *f, const char *name);
static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static IrBlock *emit_init_plan(IrFunction *f, IrBlock *bb, IrOperand base, InitPlan *plan,
                               FuncBuilder *ctx);
static bool serialize_init_plan(IrGlobal *g, InitPlan *plan, IrModule *mod, StrMap *global_map,
                                U64Map *static_map, Arena *arena);
static ExprResult build_deref_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static ExprResult build_addr_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);
static ExprResult build_subscript_expr(ASTSubscriptExpr *se, IrFunction *f, IrBlock *bb,
                                       FuncBuilder *ctx);
static ExprResult build_string_literal_expr(ASTStringLiteral *sl, IrFunction *f, IrBlock *bb,
                                            FuncBuilder *ctx);
static ExprResult build_member_lvalue(ASTMemberAccess *ma, IrFunction *f, IrBlock *bb,
                                      FuncBuilder *ctx);
static ExprResult build_member_access_expr(ASTMemberAccess *ma, IrFunction *f, IrBlock *bb,
                                           FuncBuilder *ctx);
static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx);

static void ir_error(ASTNode *node, const char *fmt, ...)
{
    Loc loc = node->loc;
    fprintf(stderr, "%s:%u:%u: [ir] error: ", loc.file, loc.line, loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static IrOperand *box_operand(FuncBuilder *ctx, IrOperand op)
{
    IrOperand *p = arena_alloc(ctx->mod->arena, sizeof(IrOperand), sizeof(IrOperand));
    *p = op;
    return p;
}

static u32 alloc_vreg_from_type(FuncBuilder *ctx, Type *type)
{
    u8 width = type->width / 8;
    ASSERT(width == 1 || width == 2 || width == 4 || width == 8);
    return ir_alloc_vreg(ctx->mod, width, type_is_signed(type));
}

/* Records and arrays are memory objects: addressed by pointer, never loaded
   as SSA scalars. */
static bool type_is_memory(Type *t)
{
    return type_is_record(t) || type_is_array(t);
}

/* Memory objects' SSA slot is a pointer to their storage. */
static Type *var_ssa_type(Type *t)
{
    if (type_is_memory(t))
    {
        return type_ptr(t);
    }
    return type_decay(t);
}

static u32 alloc_vreg_for_var(FuncBuilder *ctx, Type *type)
{
    return alloc_vreg_from_type(ctx, var_ssa_type(type));
}

static inline Type *node_type(ASTNode *n)
{
    return n->expr_type ? n->expr_type : type_int();
}

static IrOperand promote_to(FuncBuilder *ctx, IrBlock *bb, IrOperand val, Type *src_type,
                            Type *target_type)
{
    u8 src_w = src_type->width / 8;
    u8 tgt_w = target_type->width / 8;
    if (target_type->kind == TYPE_BOOL && src_type->kind != TYPE_BOOL)
    {
        /* §6.3.1.2: _Bool holds (val != 0) — ICMP_NE, TRUNC'd when wider than 1. */
        u32 cmp = alloc_vreg_from_type(ctx, src_type);
        ir_emit_binop(bb, OP_ICMP_NE, cmp, val, ir_operand_imm(0));
        if (src_w == 1)
        {
            return ir_operand_vreg(cmp);
        }
        u32 dst = alloc_vreg_from_type(ctx, target_type);
        ir_emit_unary(bb, OP_TRUNC, dst, ir_operand_vreg(cmp));
        return ir_operand_vreg(dst);
    }
    if (src_type->kind == target_type->kind || src_w == tgt_w)
    {
        return val;
    }
    if (val.is_imm)
    {
        u32 src_vreg = alloc_vreg_from_type(ctx, src_type);
        ir_emit_unary(bb, type_is_signed(src_type) ? OP_SEXT : OP_ZEXT, src_vreg, val);
        val = ir_operand_vreg(src_vreg);
    }
    if (tgt_w > src_w)
    {
        u32 dst = alloc_vreg_from_type(ctx, target_type);
        IrOpcode op = type_is_signed(src_type) ? OP_SEXT : OP_ZEXT;
        ir_emit_unary(bb, op, dst, val);
        return ir_operand_vreg(dst);
    }
    else if (tgt_w < src_w)
    {
        u32 dst = alloc_vreg_from_type(ctx, target_type);
        ir_emit_unary(bb, OP_TRUNC, dst, val);
        return ir_operand_vreg(dst);
    }
    return val;
}

static bool is_comparison_op(BinOpKind op)
{
    return op >= BIN_EQ && op <= BIN_GE;
}

static bool is_shift_op(BinOpKind op)
{
    return op == BIN_SHL || op == BIN_SHR;
}

static bool is_divrem_op(BinOpKind op)
{
    return op == BIN_DIV || op == BIN_REM;
}

static u32 alloc_phi_vreg(FuncBuilder *ctx, ASTVarDecl *var)
{
    /* A phi slot holds the var's SSA shape: a decayed/pointed value. */
    return alloc_vreg_for_var(ctx, var->type);
}

static BlockLocals *get_block_locals(FuncBuilder *ctx, IrBlock *bb)
{
    BlockLocals *bl = u64map_get(ctx->block_locals, (u64) (uintptr_t) bb);
    if (!bl)
    {
        bl = arena_alloc(ctx->mod->arena, sizeof(BlockLocals), sizeof(void *));
        bl->locals = u64map_new(ctx->mod->arena);
        bl->pending_phis = vec_new(ctx->mod->arena);
        u64map_set(ctx->block_locals, (u64) (uintptr_t) bb, bl);
    }
    return bl;
}

static IrBlock *new_block(IrFunction *f, const char *prefix)
{
    size_t idx = vec_size(f->blocks);
    size_t len = strlen(prefix);
    char *buf = arena_alloc(f->arena, len + 32, sizeof(char));
    snprintf(buf, len + 32, "%s_%zu", prefix, idx);
    return ir_func_add_block(f, buf);
}

static bool is_terminated(IrBlock *bb)
{
    size_t n = vec_size(bb->instrs);
    if (n == 0)
    {
        return false;
    }
    IrInstr *last = (IrInstr *) vec_get(bb->instrs, n - 1);
    return last->opcode == OP_RET || last->opcode == OP_UNREACHABLE || last->opcode == OP_BR ||
           last->opcode == OP_BRCOND || last->opcode == OP_SWITCH;
}

static void jump(IrBlock *from, IrBlock *to)
{
    ir_emit_br(from, to->label);
    vec_push(to->preds, from);
}

static void cond_jump(IrBlock *from, IrOperand cond, IrBlock *then_bb, IrBlock *else_bb)
{
    ir_emit_brcond(from, cond, then_bb->label, else_bb->label);
    vec_push(then_bb->preds, from);
    vec_push(else_bb->preds, from);
}

static void declare_pred(IrBlock *bb, IrBlock *pred)
{
    vec_push(bb->preds, pred);
}

/* --- scalar address-of spill machinery ---
   An address-taken block-scope auto becomes memory-resident: a function-entry
   OP_ALLOCA slot (keyed in spill_slots) that every read/write loads/stores,
   bypassing the SSA stack — loads see current memory, so no PHIs are needed. */

static bool is_spillable_var(ASTVarDecl *decl)
{
    return decl->is_block_scope && decl->storage == SC_NONE && !type_is_array(decl->type) &&
           !type_is_record(decl->type);
}

static IrOperand *spill_slot(FuncBuilder *ctx, ASTVarDecl *var)
{
    return u64map_get(ctx->spill_slots, (u64) (uintptr_t) var);
}

/* Allocate each address-taken auto's slot in the entry block: hoisted so slot
   addresses are invariant and dominate every use. */
static void emit_spill_allocas(FuncBuilder *ctx, IrBlock *entry)
{
    size_t n = vec_size(ctx->spilled);
    for (size_t i = 0; i < n; i++)
    {
        ASTVarDecl *vd = (ASTVarDecl *) vec_get(ctx->spilled, i);
        u32 slot = alloc_vreg_from_type(ctx, type_ptr(vd->type));
        ir_emit_alloca(entry, slot, type_sizeof(vd->type));
        IrOperand *op = box_operand(ctx, ir_operand_vreg(slot));
        u64map_set(ctx->spill_slots, (u64) (uintptr_t) vd, op);
    }
}

static IrOperand read_variable(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb)
{
    IrOperand *slot = spill_slot(ctx, var);
    if (slot)
    {
        u32 dst = alloc_vreg_from_type(ctx, var->type);
        ir_emit_load(bb, dst, *slot);
        return ir_operand_vreg(dst);
    }
    BlockLocals *bl = get_block_locals(ctx, bb);
    IrOperand *p = u64map_get(bl->locals, (u64) (uintptr_t) var);
    if (p)
    {
        return *p;
    }
    return resolve_variable(ctx, var, bb);
}

static IrOperand resolve_variable(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb)
{
    BlockLocals *bl = get_block_locals(ctx, bb);
    size_t npreds = vec_size(bb->preds);
    IrOperand val;
    if (npreds == 0)
    {
        val = ir_operand_imm(0); /* undefined; semantic rejected the use */
    }
    else if (npreds == 1 && !bb->is_loop_header)
    {
        /* Single pred, no merge: reuse its value. */
        val = read_variable(ctx, var, (IrBlock *) vec_get(bb->preds, 0));
    }
    else if (bb->sealed)
    {
        val = build_phi(ctx, var, bb);
    }
    else
    {
        /* Unsealed merge: register a placeholder PHI filled on seal. */
        u32 dst = alloc_phi_vreg(ctx, var);
        PendingPhi *ip = arena_alloc(ctx->mod->arena, sizeof(PendingPhi), sizeof(void *));
        ip->var = var;
        ip->vreg = dst;
        vec_push(bl->pending_phis, ip);
        val = ir_operand_vreg(dst);
    }
    u64map_set(bl->locals, (u64) (uintptr_t) var, box_operand(ctx, val));
    return val;
}

static void write_variable(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb, IrOperand val)
{
    IrOperand *slot = spill_slot(ctx, var);
    if (slot)
    {
        ir_emit_store(bb, val, *slot, var->type->size);
        return;
    }
    BlockLocals *bl = get_block_locals(ctx, bb);
    u64map_set(bl->locals, (u64) (uintptr_t) var, box_operand(ctx, val));
}

static IrInstr *emit_phi_at_start(IrBlock *bb, u32 dst, u32 nentries)
{
    IrInstr *phi = ir_emit_phi(bb, dst, nentries);
    IrInstr *last = (IrInstr *) vec_pop(bb->instrs);
    ASSERT(last == phi);
    vec_insert(bb->instrs, 0, phi);
    return phi;
}

static void fill_phi_entries(FuncBuilder *ctx, IrBlock *bb, ASTVarDecl *var, IrInstr *phi)
{
    size_t n = vec_size(bb->preds);
    for (size_t e = 0; e < n; e++)
    {
        IrBlock *pred = (IrBlock *) vec_get(bb->preds, e);
        IrOperand pval = read_variable(ctx, var, pred);
        ir_phi_add_entry(phi, pval, pred);
    }
}

static IrOperand build_phi(FuncBuilder *ctx, ASTVarDecl *var, IrBlock *bb)
{
    BlockLocals *bl = get_block_locals(ctx, bb);
    u32 dst = alloc_phi_vreg(ctx, var);
    IrInstr *phi = emit_phi_at_start(bb, dst, (u32) vec_size(bb->preds));
    /* Register before filling: a loop header's back-edge can resolve through
       this block while its PHI is being built, which must terminate. */
    u64map_set(bl->locals, (u64) (uintptr_t) var, box_operand(ctx, ir_operand_vreg(dst)));
    fill_phi_entries(ctx, bb, var, phi);
    return ir_operand_vreg(dst);
}

static void insert_phi(FuncBuilder *ctx, IrBlock *bb, ASTVarDecl *var, u32 dst)
{
    u32 nentries = (u32) vec_size(bb->preds);
    IrInstr *phi = emit_phi_at_start(bb, dst, nentries);
    fill_phi_entries(ctx, bb, var, phi);
}

static void seal_block(FuncBuilder *ctx, IrBlock *bb)
{
    if (bb->sealed)
    {
        return;
    }
    bb->sealed = true;

    BlockLocals *bl = get_block_locals(ctx, bb);
    Vec *phis = bl->pending_phis;
    size_t n = vec_size(phis);
    for (size_t i = 0; i < n; i++)
    {
        PendingPhi *ip = (PendingPhi *) vec_get(phis, i);
        insert_phi(ctx, bb, ip->var, ip->vreg);
    }
}

static const IrOpcode binop_ir[] = {
    [BIN_ADD] = OP_ADD, [BIN_SUB] = OP_SUB, [BIN_MUL] = OP_MUL, [BIN_AND] = OP_AND,
    [BIN_OR] = OP_OR,   [BIN_XOR] = OP_XOR, [BIN_SHL] = OP_SHL, [BIN_SHR] = OP_ASHR,
    /* BIN_EQ..BIN_GE, BIN_DIV, BIN_REM, BIN_SHR are filled in by arith_opcode */
};

static const IrOpcode unary_opcodes[] = {
    [UN_NEG] = OP_NEG,
    [UN_BIT_NOT] = OP_NOT,
};

static void emit_unary_op(IrBlock *bb, u32 dst, UnaryOpKind op, IrOperand src, ASTNode *node)
{
    IrOpcode code = unary_opcodes[op];
    if (code == 0)
    {
        ir_error(node, "unsupported unary operator");
        return;
    }
    ir_emit_unary(bb, code, dst, src);
}

static ExprResult expr_result(IrOperand value, IrBlock *bb)
{
    ExprResult r = {value, bb};
    return r;
}

static ExprResult build_int_literal_expr(ASTIntLiteral *lit, IrFunction *f, IrBlock *bb,
                                         FuncBuilder *ctx)
{
    (void) f;
    (void) ctx;
    return expr_result(ir_operand_imm(lit->value), bb);
}

static ExprResult build_ident_expr(ASTIdent *id, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    if (id->is_func)
    {
        /* A function designator (§6.3.2.1p4) is its address: no load. */
        return expr_result(ir_operand_func(id->name), bb);
    }
    ASTVarDecl *decl = id->decl;
    ASSERT(decl != NULL);
    if (decl->is_block_scope && decl->storage != SC_STATIC)
    {
        return expr_result(read_variable(ctx, decl, bb), bb);
    }
    u32 midx = var_global_index(ctx, decl);
    if (midx != NO_VREG)
    {
        IrGlobal *g = (IrGlobal *) vec_get(ctx->mod->globals, midx);
        IrOperand addr = ir_operand_global(midx);
        if (type_is_memory(g->type))
        {
            return expr_result(addr, bb);
        }
        u32 dst = alloc_vreg_from_type(ctx, g->type);
        ir_emit_load(bb, dst, addr);
        return expr_result(ir_operand_vreg(dst), bb);
    }
    return expr_result(ir_operand_imm(0), bb);
}

static ExprResult build_short_circuit(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb,
                                      FuncBuilder *ctx)
{
    bool is_or = be->op == BIN_LOG_OR;
    ExprResult left = build_expr(be->left, f, bb, ctx);
    IrBlock *true_bb = new_block(f, is_or ? "lor_true" : "land_true");
    IrBlock *false_bb = new_block(f, is_or ? "lor_false" : "land_false");
    IrBlock *right_bb = new_block(f, is_or ? "lor_rhs" : "land_rhs");
    IrBlock *merge_bb = new_block(f, is_or ? "lor_merge" : "land_merge");

    if (is_or)
    {
        cond_jump(left.block, left.value, true_bb, right_bb);
    }
    else
    {
        cond_jump(left.block, left.value, right_bb, false_bb);
    }

    ExprResult right = build_expr(be->right, f, right_bb, ctx);
    cond_jump(right.block, right.value, true_bb, false_bb);

    jump(true_bb, merge_bb);
    jump(false_bb, merge_bb);

    /* Seal branches first so merge inputs come from sealed preds. */
    seal_block(ctx, left.block);
    seal_block(ctx, right_bb);
    seal_block(ctx, false_bb);
    seal_block(ctx, true_bb);

    u32 dst = alloc_vreg_from_type(ctx, type_int());
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, ir_operand_imm(1), true_bb);
    ir_phi_add_entry(phi, ir_operand_imm(0), false_bb);
    seal_block(ctx, merge_bb);

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

static ExprResult build_ternary_expr(ASTTernaryExpr *te, IrFunction *f, IrBlock *bb,
                                     FuncBuilder *ctx)
{
    ExprResult cond = build_expr(te->cond, f, bb, ctx);

    IrBlock *then_bb = new_block(f, "tern_then");
    IrBlock *else_bb = new_block(f, "tern_else");
    IrBlock *merge_bb = new_block(f, "tern_merge");

    cond_jump(cond.block, cond.value, then_bb, else_bb);

    ExprResult then_val = build_expr(te->then_expr, f, then_bb, ctx);
    if (!is_terminated(then_val.block))
    {
        jump(then_val.block, merge_bb);
    }

    ExprResult else_val = build_expr(te->else_expr, f, else_bb, ctx);
    if (!is_terminated(else_val.block))
    {
        jump(else_val.block, merge_bb);
    }

    Type *tern_type = node_type((ASTNode *) te);
    u32 dst = alloc_vreg_from_type(ctx, tern_type);
    IrInstr *phi = emit_phi_at_start(merge_bb, dst, 2);
    ir_phi_add_entry(phi, then_val.value, then_val.block);
    ir_phi_add_entry(phi, else_val.value, else_val.block);
    seal_block(ctx, merge_bb);

    return expr_result(ir_operand_vreg(dst), merge_bb);
}

/* Element address of a subscript: index promoted to long, GEP'd by size. */
static ExprResult build_subscript_addr(ASTSubscriptExpr *se, IrFunction *f, IrBlock *bb,
                                       FuncBuilder *ctx)
{
    ExprResult base = build_expr(se->array, f, bb, ctx);
    ExprResult index = build_expr(se->index, f, base.block, ctx);
    bb = index.block;
    Type *elem = type_deref(type_decay(node_type(se->array)));
    IrOperand idx = promote_to(ctx, bb, index.value, node_type(se->index), type_long());
    u32 addr = alloc_vreg_from_type(ctx, type_ptr(elem));
    ir_emit_gep(bb, addr, base.value, idx, elem->size);
    return expr_result(ir_operand_vreg(addr), bb);
}

static LvalueResult bad_lvalue(IrBlock *bb)
{
    LvalueResult lv = {.block = bb, .failed = true};
    return lv;
}

static LvalueResult mem_lvalue(IrOperand addr, Type *type, IrBlock *bb)
{
    LvalueResult lv = {.block = bb};
    lv.slot.is_ssa = false;
    lv.slot.addr = addr;
    lv.slot.type = type;
    return lv;
}

/* Lower a write target into an lvalue slot. The address is computed exactly
   once; operators never re-evaluate it. */
static LvalueResult build_lvalue_slot(ASTNode *target, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (target->kind)
    {
        case AST_IDENT:
        {
            ASTIdent *id = ast_as(ASTIdent, target);
            ASTVarDecl *decl = id->decl;
            ASSERT(decl != NULL);
            if (decl->is_block_scope && decl->storage != SC_STATIC)
            {
                if (type_is_memory(decl->type))
                {
                    return mem_lvalue(read_variable(ctx, decl, bb), decl->type, bb);
                }
                LvalueResult lv = {.block = bb};
                lv.slot.is_ssa = true;
                lv.slot.decl = decl;
                lv.slot.type = decl->type;
                return lv;
            }
            u32 midx = var_global_index(ctx, decl);
            if (midx != NO_VREG)
            {
                return mem_lvalue(ir_operand_global(midx),
                                  ((IrGlobal *) vec_get(ctx->mod->globals, midx))->type, bb);
            }
            ir_error(target, "unknown variable '%s'", id->name);
            return bad_lvalue(bb);
        }
        case AST_MEMBER_ACCESS:
        {
            ASTMemberAccess *ma = ast_as(ASTMemberAccess, target);
            ExprResult lv = build_member_lvalue(ma, f, bb, ctx);
            return mem_lvalue(lv.value, ma->field_type, lv.block);
        }
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, target);
            if (ue->op == UN_DEREF)
            {
                ExprResult ptr = build_expr(ue->operand, f, bb, ctx);
                return mem_lvalue(ptr.value, node_type(target), ptr.block);
            }
            break;
        }
        case AST_SUBSCRIPT_EXPR:
        {
            ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, target);
            ExprResult addr = build_subscript_addr(se, f, bb, ctx);
            return mem_lvalue(addr.value, type_deref(type_decay(node_type(se->array))), addr.block);
        }
        default:
            break;
    }
    ir_error(target, "assignment target must be a variable, dereference, or subscript");
    return bad_lvalue(bb);
}

/* Read a lowered lvalue: SSA scalars via read_variable (or their spill slot);
   memory targets loaded at the slot type's width. */
static IrOperand load_lvalue(FuncBuilder *ctx, IrBlock *bb, LvalueSlot *slot)
{
    if (slot->is_ssa)
    {
        return read_variable(ctx, slot->decl, bb);
    }
    u32 dst = alloc_vreg_from_type(ctx, slot->type);
    ir_emit_load(bb, dst, slot->addr);
    return ir_operand_vreg(dst);
}

/* Store into a lowered lvalue: records via OP_MEMCPY (their value is a pointer
   to source storage); scalars via OP_STORE; SSA scalars via write_variable. */
static IrBlock *store_lvalue(FuncBuilder *ctx, IrBlock *bb, LvalueSlot *slot, IrOperand val)
{
    if (slot->is_ssa)
    {
        write_variable(ctx, slot->decl, bb, val);
        return bb;
    }
    if (type_is_record(slot->type))
    {
        ir_emit_memcpy(bb, slot->addr, val, slot->type->size);
        return bb;
    }
    ir_emit_store(bb, val, slot->addr, slot->type->size);
    return bb;
}

static ExprResult build_assign_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    LvalueResult lv = build_lvalue_slot(be->left, f, bb, ctx);
    if (lv.failed)
    {
        return expr_result(ir_operand_imm(0), lv.block);
    }
    bb = lv.block;
    ExprResult right = build_expr(be->right, f, bb, ctx);
    bb = right.block;
    Type *lhs_type = lv.slot.type;
    if (type_is_array(lhs_type))
    {
        ir_error(&be->base, "array is not assignable");
        return expr_result(ir_operand_imm(0), bb);
    }
    if (type_is_record(lhs_type))
    {
        bb = store_lvalue(ctx, bb, &lv.slot, right.value);
        return expr_result(lv.slot.addr, bb);
    }
    Type *rhs_type = node_type(be->right);
    IrOperand val = promote_to(ctx, bb, right.value, rhs_type, lhs_type);
    bb = store_lvalue(ctx, bb, &lv.slot, val);
    return expr_result(val, bb);
}

/* `++`/`--`: load old, step (pointers GEP by pointee size, arithmetic
   promotes/adjusts/rewraps), store; postfix yields old, prefix new. */
static ExprResult build_incdec_expr(ASTIncDecExpr *ie, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    LvalueResult lv = build_lvalue_slot(ie->operand, f, bb, ctx);
    if (lv.failed)
    {
        return expr_result(ir_operand_imm(0), lv.block);
    }
    bb = lv.block;
    Type *t = lv.slot.type;
    IrOperand old = load_lvalue(ctx, bb, &lv.slot);
    i64 step = ie->is_inc ? 1 : -1;

    IrOperand newv;
    if (type_is_ptr(t))
    {
        Type *elem = type_deref(t);
        u32 gep = alloc_vreg_from_type(ctx, t);
        ir_emit_gep(bb, gep, old, ir_operand_imm(step), elem->size);
        newv = ir_operand_vreg(gep);
    }
    else
    {
        Type *prom = type_promote(t);
        IrOperand pold = promote_to(ctx, bb, old, t, prom);
        u32 res = alloc_vreg_from_type(ctx, prom);
        ir_emit_binop(bb, step == 1 ? OP_ADD : OP_SUB, res, pold, ir_operand_imm(1));
        newv = promote_to(ctx, bb, ir_operand_vreg(res), prom, t);
    }

    bb = store_lvalue(ctx, bb, &lv.slot, newv);
    if (ie->is_postfix)
    {
        return expr_result(old, bb);
    }
    return expr_result(newv, bb);
}

static BinOpKind plain_op(BinOpKind op)
{
    switch (op)
    {
        case BIN_ADD_ASSIGN:
            return BIN_ADD;
        case BIN_SUB_ASSIGN:
            return BIN_SUB;
        case BIN_MUL_ASSIGN:
            return BIN_MUL;
        case BIN_DIV_ASSIGN:
            return BIN_DIV;
        case BIN_REM_ASSIGN:
            return BIN_REM;
        case BIN_SHL_ASSIGN:
            return BIN_SHL;
        case BIN_SHR_ASSIGN:
            return BIN_SHR;
        case BIN_AND_ASSIGN:
            return BIN_AND;
        case BIN_OR_ASSIGN:
            return BIN_OR;
        case BIN_XOR_ASSIGN:
            return BIN_XOR;
        default:
            return op;
    }
}

/* Usual-arithmetic promotions: pointer operands convert the non-pointer side
   to the pointer type; shift counts promote independently (§6.5.9p4/§6.5.7). */
static void promote_binop_operands(BinOpKind op, FuncBuilder *ctx, IrBlock *bb, IrOperand *lhs,
                                   IrOperand *rhs, Type **lt, Type **rt)
{
    if (!is_comparison_op(op) && !is_shift_op(op) && !is_divrem_op(op))
    {
        Type *promoted = type_common(type_promote(*lt), type_promote(*rt));
        *lhs = promote_to(ctx, bb, *lhs, *lt, promoted);
        *lt = promoted;
        *rhs = promote_to(ctx, bb, *rhs, *rt, promoted);
        *rt = promoted;
        return;
    }
    if (is_comparison_op(op) || is_divrem_op(op))
    {
        Type *promoted;
        if (type_is_ptr(*lt) && !type_is_ptr(*rt))
        {
            promoted = *lt;
        }
        else if (type_is_ptr(*rt) && !type_is_ptr(*lt))
        {
            promoted = *rt;
        }
        else if (type_is_ptr(*lt) || type_is_ptr(*rt))
        {
            promoted = *lt;
        }
        else
        {
            promoted = type_common(type_promote(*lt), type_promote(*rt));
        }
        *lhs = promote_to(ctx, bb, *lhs, *lt, promoted);
        *lt = promoted;
        *rhs = promote_to(ctx, bb, *rhs, *rt, promoted);
        *rt = promoted;
        return;
    }
    /* Shifts: an unpromoted char would shift at its own width with garbage
       high bits (`char -8 >> 1` → 124 instead of -4). */
    *lhs = promote_to(ctx, bb, *lhs, *lt, type_promote(*lt));
    *lt = type_promote(*lt);
    *rhs = promote_to(ctx, bb, *rhs, *rt, type_promote(*rt));
    *rt = type_promote(*rt);
}

/* Select the IR opcode for a promoted binary op. */
static IrOpcode arith_opcode(BinOpKind op, Type *lt)
{
    IrOpcode opcode = binop_ir[op];
    if (is_comparison_op(op))
    {
        bool unsig = type_is_unsigned(lt);
        switch (op)
        {
            case BIN_EQ:
                return OP_ICMP_EQ;
            case BIN_NE:
                return OP_ICMP_NE;
            case BIN_LT:
                return unsig ? OP_ICMP_ULT : OP_ICMP_SLT;
            case BIN_GT:
                return unsig ? OP_ICMP_UGT : OP_ICMP_SGT;
            case BIN_LE:
                return unsig ? OP_ICMP_ULE : OP_ICMP_SLE;
            case BIN_GE:
                return unsig ? OP_ICMP_UGE : OP_ICMP_SGE;
            default:
                return opcode;
        }
    }
    if (is_divrem_op(op))
    {
        return type_is_unsigned(lt) ? (op == BIN_DIV ? OP_UDIV : OP_UREM)
                                    : (op == BIN_DIV ? OP_SDIV : OP_SREM);
    }
    if (op == BIN_SHR)
    {
        return type_is_unsigned(lt) ? OP_LSHR : OP_ASHR;
    }
    return opcode;
}

static ArithResult lower_arith_into(ArithSpec spec, IrOperand lval, IrOperand rval, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    ArithResult ar;
    ar.value = ir_operand_imm(0);
    ar.block = bb;
    ar.result_type = spec.result_type;

    if (type_is_ptr(spec.lt) && (spec.op == BIN_ADD || spec.op == BIN_SUB) && !type_is_ptr(spec.rt))
    {
        /* Pointer arithmetic scales by the pointee size (§6.5.6p8). */
        IrOperand lhs = lval;
        IrOperand rhs = promote_to(ctx, bb, rval, spec.rt, type_long());
        if (spec.op == BIN_SUB)
        {
            u32 neg_vreg = alloc_vreg_from_type(ctx, type_long());
            ir_emit_unary(bb, OP_NEG, neg_vreg, rhs);
            rhs = ir_operand_vreg(neg_vreg);
        }
        Type *elem = type_deref(spec.lt);
        u32 gep_vreg = alloc_vreg_from_type(ctx, spec.lt);
        ir_emit_gep(bb, gep_vreg, lhs, rhs, elem->size);
        ar.value = ir_operand_vreg(gep_vreg);
        return ar;
    }

    IrOperand lhs = lval;
    IrOperand rhs = rval;
    Type *lt = spec.lt;
    Type *rt = spec.rt;
    promote_binop_operands(spec.op, ctx, bb, &lhs, &rhs, &lt, &rt);

    IrOpcode opcode = arith_opcode(spec.op, lt);
    if (opcode == 0)
    {
        ir_error(spec.node, "unsupported binary operator");
        return ar;
    }
    u32 dst = alloc_vreg_from_type(ctx, spec.result_type);
    ir_emit_binop(bb, opcode, dst, lhs, rhs);
    ar.value = ir_operand_vreg(dst);
    return ar;
}

static ExprResult build_arith_binop_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb,
                                         FuncBuilder *ctx)
{
    ExprResult left = build_expr(be->left, f, bb, ctx);
    ExprResult right = build_expr(be->right, f, left.block, ctx);

    ArithSpec spec = {.op = be->op,
                      .lt = type_decay(node_type(be->left)),
                      .rt = type_decay(node_type(be->right)),
                      .result_type = node_type((ASTNode *) be),
                      .node = (ASTNode *) be};
    ArithResult ar = lower_arith_into(spec, left.value, right.value, right.block, ctx);
    return expr_result(ar.value, ar.block);
}

/* Compound assignment (§6.5.16.2): load once, run the plain op's lowering,
   rewrap to the LHS type, store, and yield the new value. */
static ExprResult build_compound_assign(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb,
                                        FuncBuilder *ctx)
{
    LvalueResult lv = build_lvalue_slot(be->left, f, bb, ctx);
    if (lv.failed)
    {
        return expr_result(ir_operand_imm(0), lv.block);
    }
    bb = lv.block;
    Type *lt = lv.slot.type;
    IrOperand cur = load_lvalue(ctx, bb, &lv.slot);
    ExprResult right = build_expr(be->right, f, bb, ctx);
    bb = right.block;
    Type *rt = type_decay(node_type(be->right));

    ArithSpec spec = {.op = plain_op(be->op),
                      .lt = lt,
                      .rt = rt,
                      .result_type =
                          type_is_ptr(lt) ? lt : type_common(type_promote(lt), type_promote(rt)),
                      .node = (ASTNode *) be};

    ArithResult ar = lower_arith_into(spec, cur, right.value, bb, ctx);
    bb = ar.block;

    IrOperand newv = promote_to(ctx, bb, ar.value, ar.result_type, lt);
    bb = store_lvalue(ctx, bb, &lv.slot, newv);
    return expr_result(newv, bb);
}

static ExprResult build_unary_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (ue->op)
    {
        case UN_DEREF:
            return build_deref_expr(ue, f, bb, ctx);
        case UN_ADDR:
            return build_addr_expr(ue, f, bb, ctx);
        default:
            break;
    }
    ExprResult src = build_expr(ue->operand, f, bb, ctx);
    Type *result_type = node_type((ASTNode *) ue);
    u32 dst = alloc_vreg_from_type(ctx, result_type);
    if (ue->op == UN_LOG_NOT)
    {
        ir_emit_binop(src.block, OP_ICMP_EQ, dst, src.value, ir_operand_imm(0));
    }
    else
    {
        Type *op_type = node_type(ue->operand);
        Type *promoted = type_promote(op_type);
        IrOperand promoted_op = promote_to(ctx, src.block, src.value, op_type, promoted);
        emit_unary_op(src.block, dst, ue->op, promoted_op, &ue->base);
    }
    return expr_result(ir_operand_vreg(dst), src.block);
}

/* An anonymous object whose slot is allocated and plan-applied (§6.5.2.5). */
static ExprResult compound_literal_slot(ASTCompoundLiteral *cl, IrFunction *f, IrBlock *bb,
                                        FuncBuilder *ctx)
{
    u32 slot = alloc_vreg_from_type(ctx, type_ptr(cl->type));
    ir_emit_alloca(bb, slot, cl->type->size);
    bb = emit_init_plan(f, bb, ir_operand_vreg(slot), cl->plan, ctx);
    return expr_result(ir_operand_vreg(slot), bb);
}

/* §6.5.2.5 compound literal: yield the slot address (memory) or a loaded
   scalar. */
static ExprResult build_compound_literal_expr(ASTCompoundLiteral *cl, IrFunction *f, IrBlock *bb,
                                              FuncBuilder *ctx)
{
    ExprResult slot = compound_literal_slot(cl, f, bb, ctx);
    bb = slot.block;
    Type *ty = type_unqual(cl->type);
    if (type_is_memory(ty))
    {
        return slot;
    }
    if (ty->kind == TYPE_VOID)
    {
        return expr_result(ir_operand_imm(0), bb);
    }
    u32 dst = alloc_vreg_from_type(ctx, ty);
    ir_emit_load(bb, dst, slot.value);
    return expr_result(ir_operand_vreg(dst), bb);
}

/* §6.5.4 cast: `(void)` discards; integer/pointer converts via promote_to;
   pointer→pointer is a reinterpretation, so no instruction. */
static ExprResult build_cast_expr(ASTCastExpr *ce, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult src = build_expr(ce->operand, f, bb, ctx);
    bb = src.block;
    Type *target = ce->target_type;
    if (target->kind == TYPE_VOID)
    {
        return expr_result(ir_operand_imm(0), bb);
    }
    Type *src_type = node_type(ce->operand);
    if (type_is_ptr(target) && type_is_ptr(src_type))
    {
        return expr_result(src.value, bb);
    }
    return expr_result(promote_to(ctx, bb, src.value, src_type, target), bb);
}

/* `__builtin_va_arg`: OP_VA_ARG reads the full 8-byte slot (the backends
   advance ap), then promote_to converts to the requested type. */
static ExprResult build_va_arg_expr(ASTVaArgExpr *va, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult ap = build_expr(va->ap, f, bb, ctx);
    Type *target = type_rvalue(va->type);
    u32 raw = alloc_vreg_from_type(ctx, type_long());
    ir_emit_va_arg(ap.block, raw, ap.value);
    return expr_result(promote_to(ctx, ap.block, ir_operand_vreg(raw), type_long(), target),
                       ap.block);
}

/* va_start/va_end record ap and the offset; the register spill lives in the
   backends. */
static ExprResult build_va_builtin(ASTCallExpr *ce, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (strcmp(ce->callee, "__builtin_va_start") == 0)
    {
        ExprResult ap = build_expr((ASTNode *) vec_get(ce->args, 0), f, bb, ctx);
        (void) build_expr((ASTNode *) vec_get(ce->args, 1), f, ap.block, ctx);
        i64 n_params = (i64) vec_size(f->params);
        i64 gp = n_params * 8 < 48 ? n_params * 8 : 48;
        i64 skip = n_params > 6 ? (n_params - 6) * 8 : 0;
        ir_emit_va_start(ap.block, ap.value, skip, gp);
        return expr_result(ir_operand_imm(0), ap.block);
    }
    if (strcmp(ce->callee, "__builtin_va_end") == 0)
    {
        ExprResult ap = build_expr((ASTNode *) vec_get(ce->args, 0), f, bb, ctx);
        ir_emit_va_end(ap.block, ap.value);
        return expr_result(ir_operand_imm(0), ap.block);
    }
    ir_error((ASTNode *) ce, "unknown builtin '%s'", ce->callee);
    ctx->failed = true;
    return expr_result(ir_operand_imm(0), bb);
}

/* Lower one call argument: record args are copied into a fresh temp and passed
   by pointer; the rest convert to the parameter's type. */
static IrBlock *lower_call_arg(FuncBuilder *ctx, IrFunction *f, ASTNode *arg, Type *param_type,
                               IrBlock *bb, IrOperand *out)
{
    ExprResult arg_res = build_expr(arg, f, bb, ctx);
    bb = arg_res.block;
    Type *arg_type = arg->expr_type;
    if (type_is_record(arg_type))
    {
        u32 tmp = alloc_vreg_for_var(ctx, arg_type);
        ir_emit_alloca(bb, tmp, arg_type->size);
        ir_emit_memcpy(bb, ir_operand_vreg(tmp), arg_res.value, arg_type->size);
        *out = ir_operand_vreg(tmp);
        return bb;
    }
    *out = promote_to(ctx, bb, arg_res.value, arg_type, param_type);
    return bb;
}

/* The parameter type for arg `i`: the signature param, a promotion-ranked
   variadic tail arg (§6.5.2.2p7), or the arg type. */
static Type *call_param_type(Type *callee_type, size_t i, Type *arg_type)
{
    size_t n_declared = vec_size(callee_type->func.params);
    if (i < n_declared)
    {
        return (Type *) vec_get(callee_type->func.params, i);
    }
    return callee_type->func.is_variadic ? type_promote(arg_type) : arg_type;
}

static ExprResult build_call_expr(ASTCallExpr *ce, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    /* Resolve the callee: a named function (direct) or a runtime value
       (indirect). */
    Type *callee_type;
    IrOperand indirect_callee = ir_operand_imm(0);
    bool indirect = ce->callee_expr != NULL;
    if (indirect)
    {
        ExprResult cres = build_expr(ce->callee_expr, f, bb, ctx);
        bb = cres.block;
        indirect_callee = cres.value;
        callee_type = node_type(ce->callee_expr);
        if (type_is_function(callee_type))
        {
            callee_type = type_decay(callee_type);
        }
        ASSERT(callee_type && type_is_ptr(callee_type));
        callee_type = type_deref(callee_type);
    }
    else
    {
        callee_type = strmap_get(ctx->func_types, ce->callee);
        if (!callee_type)
        {
            /* A compiler builtin; a user definition with the same name wins
               and never reaches this branch. */
            return build_va_builtin(ce, f, bb, ctx);
        }
    }
    ASSERT(callee_type->kind == TYPE_FUNC);
    Type *callee_ret = callee_type->func.ret;
    bool is_variadic = callee_type->func.is_variadic;

    /* By-memory convention: a record return is written through a hidden sret
       pointer; record args pass by pointer. */
    bool sret = type_is_record(callee_ret);
    u32 nargs = (u32) vec_size(ce->args);
    u32 total_args = (sret ? 1 : 0) + nargs;
    IrOperand *args =
        arena_alloc(ctx->mod->arena, total_args * sizeof(IrOperand), sizeof(IrOperand));

    u32 sret_vreg = NO_VREG;
    if (sret)
    {
        sret_vreg = alloc_vreg_for_var(ctx, callee_ret);
        ir_emit_alloca(bb, sret_vreg, callee_ret->size);
        args[0] = ir_operand_vreg(sret_vreg);
    }

    for (u32 i = 0; i < nargs; i++)
    {
        ASTNode *arg = (ASTNode *) vec_get(ce->args, i);
        u32 slot = sret ? i + 1 : i;
        bb = lower_call_arg(ctx, f, arg, call_param_type(callee_type, i, arg->expr_type), bb,
                            &args[slot]);
    }

    u32 dst;
    if (sret)
    {
        /* The result is the sret slot pointer, which we already hold. */
        dst = NO_VREG;
    }
    else
    {
        dst = callee_ret->kind == TYPE_VOID ? NO_VREG : alloc_vreg_from_type(ctx, callee_ret);
    }
    IrInstr *call;
    if (indirect)
    {
        call = ir_emit_call(bb, dst, "", total_args, args);
        ir_call_set_indirect(call, indirect_callee);
    }
    else
    {
        call = ir_emit_call(bb, dst, ce->callee, total_args, args);
    }
    ir_call_set_variadic(call, is_variadic);
    if (sret)
    {
        return expr_result(ir_operand_vreg(sret_vreg), bb);
    }
    return expr_result(dst == NO_VREG ? ir_operand_imm(0) : ir_operand_vreg(dst), bb);
}

static ExprResult build_deref_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult ptr_res = build_expr(ue->operand, f, bb, ctx);
    Type *result_type = node_type((ASTNode *) ue);
    if (type_is_function(result_type))
    {
        /* `*fp` is the designator: the address we already hold (§6.3.2.1p4). */
        return ptr_res;
    }
    if (type_is_memory(result_type))
    {
        return ptr_res;
    }
    if (result_type->kind == TYPE_VOID)
    {
        /* `void*` deref: no load and no value (semantic rejects using it). */
        return expr_result(ir_operand_imm(0), ptr_res.block);
    }
    u32 dst = alloc_vreg_from_type(ctx, result_type);
    ir_emit_load(ptr_res.block, dst, ptr_res.value);
    return expr_result(ir_operand_vreg(dst), ptr_res.block);
}

static ExprResult build_addr_expr(ASTUnaryExpr *ue, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ASTNode *operand = ue->operand;
    switch (operand->kind)
    {
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *inner = ast_as(ASTUnaryExpr, operand);
            if (inner->op == UN_DEREF)
            {
                /* `&*p`: the address of the pointed-to object is just p. */
                return build_expr(inner->operand, f, bb, ctx);
            }
            break;
        }
        case AST_SUBSCRIPT_EXPR:
            return build_subscript_addr(ast_as(ASTSubscriptExpr, operand), f, bb, ctx);
        case AST_MEMBER_ACCESS:
            return build_member_lvalue(ast_as(ASTMemberAccess, operand), f, bb, ctx);
        case AST_COMPOUND_LITERAL:
            /* The slot address without the load — the point of `&`. */
            return compound_literal_slot(ast_as(ASTCompoundLiteral, operand), f, bb, ctx);
        case AST_IDENT:
        {
            ASTIdent *id = ast_as(ASTIdent, operand);
            if (id->is_func)
            {
                /* `&f` is the function's address, identical to the designator. */
                return expr_result(ir_operand_func(id->name), bb);
            }
            ASTVarDecl *decl = id->decl;
            ASSERT(decl != NULL);
            u32 midx = var_global_index(ctx, decl);
            if (midx != NO_VREG)
            {
                return expr_result(ir_operand_global(midx), bb);
            }
            /* `&x` of an address-taken auto is its spill slot address. */
            IrOperand *slot = spill_slot(ctx, decl);
            if (slot)
            {
                return expr_result(*slot, bb);
            }
            break;
        }
        default:
            break;
    }
    /* Arrays decay to a pointer in build_expr; other operands build their value. */
    return build_expr(operand, f, bb, ctx);
}

static ExprResult build_subscript_expr(ASTSubscriptExpr *se, IrFunction *f, IrBlock *bb,
                                       FuncBuilder *ctx)
{
    ExprResult addr = build_subscript_addr(se, f, bb, ctx);
    bb = addr.block;
    Type *elem = type_deref(type_decay(node_type(se->array)));
    if (type_is_memory(elem) || elem->kind == TYPE_VOID)
    {
        return addr;
    }
    u32 dst = alloc_vreg_from_type(ctx, elem);
    ir_emit_load(bb, dst, addr.value);
    return expr_result(ir_operand_vreg(dst), bb);
}

/* sizeof/alignof are compile-time constants resolved by semantic (§6.5.3.4);
   the expression value is that constant. */
static ExprResult build_size_const(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    (void) ctx;
    switch (node->kind)
    {
        case AST_SIZEOF_TYPE:
            return expr_result(ir_operand_imm((i64) ast_as(ASTSizeofType, node)->size_value), bb);
        case AST_SIZEOF_EXPR:
            return expr_result(ir_operand_imm((i64) ast_as(ASTSizeofExpr, node)->size_value), bb);
        case AST_ALIGNOF_TYPE:
            return expr_result(ir_operand_imm((i64) ast_as(ASTAlignofType, node)->align_value), bb);
        case AST_ALIGNOF_EXPR:
            return expr_result(ir_operand_imm((i64) ast_as(ASTAlignofExpr, node)->align_value), bb);
        default:
            ir_error(node, "unsupported size expression");
            return expr_result(ir_operand_imm(0), bb);
    }
}

static u32 global_index_of(FuncBuilder *ctx, const char *name)
{
    u32 *idx = strmap_get(ctx->global_map, name);
    return idx ? *idx : NO_VREG;
}

static u32 block_static_index(FuncBuilder *ctx, ASTVarDecl *var)
{
    u32 *idx = u64map_get(ctx->static_map, (u64) (uintptr_t) var);
    return idx ? *idx : NO_VREG;
}

/* The global index of a file-scope name or a block-scope static. */
static u32 var_global_index(FuncBuilder *ctx, ASTVarDecl *decl)
{
    if (decl->is_block_scope)
    {
        return block_static_index(ctx, decl);
    }
    return global_index_of(ctx, decl->name);
}

static const u8 *encode_const_bytes(Arena *arena, i64 value, u32 size)
{
    u8 *buf = arena_alloc(arena, size, 1);
    for (u32 i = 0; i < size; i++)
    {
        buf[i] = (u8) (value >> (8 * i));
    }
    return buf;
}

/* Encode a folded constant into an object of `type`'s width; `_Bool` holds
   0/1 (§6.3.1.2). */
static const u8 *encode_object_bytes(Arena *arena, i64 value, Type *type)
{
    if (type->kind == TYPE_BOOL)
    {
        value = value != 0 ? 1 : 0;
    }
    return encode_const_bytes(arena, value, (u32) type->size);
}

/* Fold a constant expression (§6.6); semantic already resolved sizeof/alignof.
   Returns false when not constant. */
static bool fold_constant_ir(ASTNode *node, i64 *out)
{
    if (!node)
    {
        return false;
    }
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            *out = ast_as(ASTIntLiteral, node)->value;
            return true;
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *u = ast_as(ASTUnaryExpr, node);
            i64 v;
            if (!fold_constant_ir(u->operand, &v))
            {
                return false;
            }
            switch (u->op)
            {
                case UN_NEG:
                    *out = -v;
                    return true;
                case UN_BIT_NOT:
                    *out = ~v;
                    return true;
                case UN_LOG_NOT:
                    *out = !v;
                    return true;
                default:
                    return false;
            }
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *b = ast_as(ASTBinaryExpr, node);
            i64 l, r;
            if (!fold_constant_ir(b->left, &l) || !fold_constant_ir(b->right, &r))
            {
                return false;
            }
            switch (b->op)
            {
                case BIN_ADD:
                    *out = l + r;
                    return true;
                case BIN_SUB:
                    *out = l - r;
                    return true;
                case BIN_MUL:
                    *out = l * r;
                    return true;
                case BIN_DIV:
                case BIN_REM:
                    if (r == 0)
                    {
                        return false;
                    }
                    *out = b->op == BIN_DIV ? l / r : l % r;
                    return true;
                case BIN_SHL:
                case BIN_SHR:
                    if (r < 0 || r > 63)
                    {
                        return false;
                    }
                    *out = b->op == BIN_SHL ? l << r : l >> r;
                    return true;
                case BIN_AND:
                    *out = l & r;
                    return true;
                case BIN_OR:
                    *out = l | r;
                    return true;
                case BIN_XOR:
                    *out = l ^ r;
                    return true;
                case BIN_LOG_AND:
                    *out = l && r;
                    return true;
                case BIN_LOG_OR:
                    *out = l || r;
                    return true;
                case BIN_EQ:
                    *out = l == r;
                    return true;
                case BIN_NE:
                    *out = l != r;
                    return true;
                case BIN_LT:
                    *out = l < r;
                    return true;
                case BIN_GT:
                    *out = l > r;
                    return true;
                case BIN_LE:
                    *out = l <= r;
                    return true;
                case BIN_GE:
                    *out = l >= r;
                    return true;
                default:
                    return false;
            }
        }
        case AST_TERNARY_EXPR:
        {
            ASTTernaryExpr *te = ast_as(ASTTernaryExpr, node);
            i64 cond;
            if (!fold_constant_ir(te->cond, &cond))
            {
                return false;
            }
            return cond ? fold_constant_ir(te->then_expr, out)
                        : fold_constant_ir(te->else_expr, out);
        }
        case AST_SIZEOF_TYPE:
            *out = (i64) ast_as(ASTSizeofType, node)->size_value;
            return true;
        case AST_SIZEOF_EXPR:
            *out = (i64) ast_as(ASTSizeofExpr, node)->size_value;
            return true;
        case AST_ALIGNOF_TYPE:
            *out = (i64) ast_as(ASTAlignofType, node)->align_value;
            return true;
        case AST_ALIGNOF_EXPR:
            *out = (i64) ast_as(ASTAlignofExpr, node)->align_value;
            return true;
        case AST_CAST_EXPR:
        {
            ASTCastExpr *ce = ast_as(ASTCastExpr, node);
            i64 v;
            if (!fold_constant_ir(ce->operand, &v) || !type_is_integer(ce->target_type))
            {
                return false;
            }
            *out = type_reduce_int(ce->target_type, v);
            return true;
        }
        default:
            return false;
    }
}

static u32 ir_add_string_global(ASTStringLiteral *sl, IrModule *mod, Arena *arena);

static GlobalReloc *push_reloc(IrGlobal *g, Arena *arena)
{
    if (!g->relocs)
    {
        g->relocs = vec_new(arena);
    }
    GlobalReloc *r = arena_alloc(arena, sizeof(GlobalReloc), _Alignof(GlobalReloc));
    vec_push(g->relocs, r);
    return r;
}

static void ir_global_add_reloc(IrGlobal *g, u32 offset, int target, Arena *arena)
{
    GlobalReloc *r = push_reloc(g, arena);
    r->offset = offset;
    r->target = target;
    r->is_func = false;
    r->func_name = NULL;
}

/* A data-side function-address relocation: elf.c resolves it against the
   function symbol like the code-side loads. */
static void ir_global_add_func_reloc(IrGlobal *g, u32 offset, const char *func_name, Arena *arena)
{
    GlobalReloc *r = push_reloc(g, arena);
    r->offset = offset;
    r->target = -1;
    r->is_func = true;
    r->func_name = func_name;
}

static char *anon_name(Arena *arena, const char *prefix, u32 idx)
{
    char *buf = arena_alloc(arena, 32, 1);
    snprintf(buf, 32, "%s_%u", prefix, idx);
    return buf;
}

/* Resolve an address-constant leaf (§6.6p9) to a global index, -2 with
 *out_func for a function-address relocation, or -1. */
static int serializer_reloc_target(ASTNode *value, IrModule *mod, StrMap *global_map,
                                   U64Map *static_map, Arena *arena, const char **out_func);

/* An anonymous file-scope compound literal becomes an IrGlobal whose init
   bytes come from its plan (relocating against other globals / strings). */
static u32 emit_file_scope_compound(ASTCompoundLiteral *cl, IrModule *mod, Arena *arena,
                                    StrMap *global_map, U64Map *static_map)
{
    IrGlobal *g = arena_alloc(arena, sizeof(IrGlobal), sizeof(void *));
    g->name = anon_name(arena, "__anoncl", (u32) vec_size(mod->globals));
    g->type = cl->type;
    g->align = cl->type->align;
    g->relocs = NULL;
    if (!serialize_init_plan(g, cl->plan, mod, global_map, static_map, arena))
    {
        return NO_VREG;
    }
    /* Const-qualified targets land in .rodata; the anonymous object is
       internal-linkage (no user-visible name). */
    bool is_const = type_is_const(cl->type);
    g->section = is_const ? IR_SECTION_RODATA : IR_SECTION_DATA;
    g->linkage = IR_LINK_LOCAL;
    u32 idx = (u32) vec_size(mod->globals);
    vec_push(mod->globals, g);
    return idx;
}

static int serializer_reloc_target(ASTNode *value, IrModule *mod, StrMap *global_map,
                                   U64Map *static_map, Arena *arena, const char **out_func)
{
    *out_func = NULL;
    if (value->kind == AST_STRING_LITERAL)
    {
        return (int) ir_add_string_global(ast_as(ASTStringLiteral, value), mod, arena);
    }
    if (value->kind == AST_IDENT)
    {
        /* A bare function designator names its address: `int (*fp)(int) = f;` */
        ASTIdent *id = ast_as(ASTIdent, value);
        if (id->is_func)
        {
            *out_func = id->name;
            return -2; /* function-address relocation */
        }
    }
    if (value->kind == AST_UNARY_EXPR)
    {
        ASTUnaryExpr *u = ast_as(ASTUnaryExpr, value);
        if (u->op == UN_ADDR)
        {
            if (u->operand->kind == AST_COMPOUND_LITERAL)
            {
                /* `&(type){...}` serializes the literal's anonymous object. */
                return (int) emit_file_scope_compound(ast_as(ASTCompoundLiteral, u->operand), mod,
                                                      arena, global_map, static_map);
            }
            if (u->operand->kind == AST_IDENT)
            {
                ASTIdent *id = ast_as(ASTIdent, u->operand);
                if (id->is_func)
                {
                    /* `&f` is the designator's address, a function reloc. */
                    *out_func = id->name;
                    return -2;
                }
                ASTVarDecl *decl = id->decl;
                if (!decl)
                {
                    return -1;
                }
                if (decl->is_block_scope)
                {
                    if (decl->storage == SC_STATIC && static_map)
                    {
                        u32 *p = u64map_get(static_map, (u64) (uintptr_t) decl);
                        return p ? (int) *p : -1;
                    }
                    return -1; /* block-scope auto: not an address constant */
                }
                u32 *p = strmap_get(global_map, decl->name);
                return p ? (int) *p : -1;
            }
        }
    }
    return -1;
}

/* String-fill length, clamped to the array size: the NUL is stored only if
   there is room (`char s[2] = "hi"` drops it, §6.7.9p14). */
static u32 clamped_string_len(ASTStringLiteral *sl, Type *array_type)
{
    u32 need = (u32) sl->length + 1;
    u32 bound = array_type->arr.length;
    return bound ? (need < bound ? need : bound) : need;
}

/* Encode a plan into init bytes: zero-filled base, folded scalars, string
   fills, and address relocs. */
static bool serialize_init_plan(IrGlobal *g, InitPlan *plan, IrModule *mod, StrMap *global_map,
                                U64Map *static_map, Arena *arena)
{
    u8 *buf = arena_alloc(arena, plan->total_size, 1);
    memset(buf, 0, (size_t) plan->total_size);
    g->init_data = buf;
    g->init_len = (size_t) plan->total_size;

    if (!plan->writes)
    {
        return true;
    }
    size_t n = vec_size(plan->writes);
    for (size_t i = 0; i < n; i++)
    {
        InitWrite *w = (InitWrite *) vec_get(plan->writes, i);
        if (w->is_string_fill)
        {
            ASTStringLiteral *sl = ast_as(ASTStringLiteral, w->value);
            memcpy(buf + w->offset, sl->data, clamped_string_len(sl, w->type));
            continue;
        }
        const char *func_name = NULL;
        int target =
            serializer_reloc_target(w->value, mod, global_map, static_map, arena, &func_name);
        if (func_name)
        {
            ir_global_add_func_reloc(g, w->offset, func_name, arena);
            continue;
        }
        if (target >= 0)
        {
            ir_global_add_reloc(g, w->offset, target, arena);
            continue;
        }
        i64 value;
        if (!fold_constant_ir(w->value, &value))
        {
            ir_error(w->value, "initializer element is not a constant");
            return false;
        }
        const u8 *bytes = encode_object_bytes(arena, value, w->type);
        memcpy(buf + w->offset, bytes, w->type->size);
    }
    return true;
}

/* Fill `g` from a variable declaration. Returns false (error reported) when an
   initializer leaf cannot be serialized. */
static bool fill_global(IrGlobal *g, ASTVarDecl *vd, IrModule *mod, StrMap *global_map,
                        U64Map *static_map, Arena *arena)
{
    g->type = vd->type;
    g->align = vd->type->align;
    g->relocs = NULL;

    /* Const-qualified objects land in `.rodata` even when zero-initialized. */
    bool is_const = type_is_const(vd->type);

    if (vd->storage == SC_EXTERN)
    {
        g->init_data = NULL;
        g->init_len = 0;
        g->section = IR_SECTION_BSS;
        g->linkage = IR_LINK_EXTERN;
    }
    else if (vd->plan)
    {
        /* Aggregate and string initializers serialize through their plan. */
        if (!serialize_init_plan(g, vd->plan, mod, global_map, static_map, arena))
        {
            return false;
        }
        g->section = is_const ? IR_SECTION_RODATA : IR_SECTION_DATA;
        g->linkage = vd->storage == SC_STATIC ? IR_LINK_LOCAL : IR_LINK_GLOBAL;
    }
    else if (vd->init && vd->init->kind == AST_STRING_LITERAL)
    {
        /* `char *p = "..."`: 8 zero bytes + a relocation against the string
           symbol. */
        const char *unused_func = NULL;
        int str_idx =
            serializer_reloc_target(vd->init, mod, global_map, static_map, arena, &unused_func);
        g->init_data = encode_const_bytes(arena, 0, 8);
        g->init_len = 8;
        g->section = is_const ? IR_SECTION_RODATA : IR_SECTION_DATA;
        g->linkage = vd->storage == SC_STATIC ? IR_LINK_LOCAL : IR_LINK_GLOBAL;
        ir_global_add_reloc(g, 0, str_idx, arena);
    }
    else if (vd->has_const_init && vd->const_init != 0)
    {
        g->init_data = encode_object_bytes(arena, vd->const_init, vd->type);
        g->init_len = (size_t) vd->type->size;
        g->section = is_const ? IR_SECTION_RODATA : IR_SECTION_DATA;
        g->linkage = vd->storage == SC_STATIC ? IR_LINK_LOCAL : IR_LINK_GLOBAL;
    }
    else if (is_const)
    {
        /* .rodata needs real PROGBITS bytes even for zero const data. */
        g->init_data = encode_const_bytes(arena, 0, (u32) vd->type->size);
        g->init_len = (size_t) vd->type->size;
        g->section = IR_SECTION_RODATA;
        g->linkage = vd->storage == SC_STATIC ? IR_LINK_LOCAL : IR_LINK_GLOBAL;
    }
    else
    {
        g->init_data = NULL;
        g->init_len = 0;
        g->section = IR_SECTION_BSS;
        g->linkage = vd->storage == SC_STATIC ? IR_LINK_LOCAL : IR_LINK_GLOBAL;
    }
    return true;
}

static u32 emit_global_decl(ASTVarDecl *vd, IrModule *mod, Arena *arena, StrMap *global_map,
                            U64Map *static_map)
{
    IrGlobal *g = arena_alloc(arena, sizeof(IrGlobal), sizeof(void *));
    g->name = vd->name;
    if (!fill_global(g, vd, mod, global_map, static_map, arena))
    {
        return NO_VREG;
    }
    /* Index after fill_global: it may append string globals. */
    u32 idx = (u32) vec_size(mod->globals);
    vec_push(mod->globals, g);

    u32 *slot = arena_alloc(arena, sizeof(u32), sizeof(u32));
    *slot = idx;
    strmap_set(global_map, vd->name, slot);
    return idx;
}

static u32 emit_block_static(ASTVarDecl *vd, IrModule *mod, Arena *arena, U64Map *static_map,
                             StrMap *global_map)
{
    IrGlobal *g = arena_alloc(arena, sizeof(IrGlobal), sizeof(void *));
    g->name = anon_name(arena, "__static", (u32) vec_size(mod->globals));
    if (!fill_global(g, vd, mod, global_map, static_map, arena))
    {
        return NO_VREG;
    }
    u32 idx = (u32) vec_size(mod->globals);
    vec_push(mod->globals, g);

    u32 *slot = arena_alloc(arena, sizeof(u32), sizeof(u32));
    *slot = idx;
    u64map_set(static_map, (u64) (uintptr_t) vd, slot);
    return idx;
}

static u32 ir_add_string_global(ASTStringLiteral *sl, IrModule *mod, Arena *arena)
{
    u32 idx = (u32) vec_size(mod->globals);
    IrGlobal *g = arena_alloc(arena, sizeof(IrGlobal), sizeof(void *));
    g->name = anon_name(arena, "__str", idx);
    g->type = type_array(type_char(), sl->length + 1);
    g->init_data = (const u8 *) sl->data;
    g->init_len = sl->length + 1;
    g->align = 1;
    g->section = IR_SECTION_RODATA;
    g->linkage = IR_LINK_LOCAL;
    g->relocs = NULL;
    vec_push(mod->globals, g);
    return idx;
}

/* A synthesized file-scope zero blob: block-scope aggregate initializers
   zero-fill their alloca slot with one OP_MEMCPY from these bytes. */
static u32 ir_add_zero_blob(IrModule *mod, Arena *arena, u32 size)
{
    u32 idx = (u32) vec_size(mod->globals);
    IrGlobal *g = arena_alloc(arena, sizeof(IrGlobal), sizeof(void *));
    g->name = anon_name(arena, "__zero", idx);
    g->type = type_array(type_char(), size);
    g->init_data = encode_const_bytes(arena, 0, size);
    g->init_len = size;
    g->align = 1;
    g->section = IR_SECTION_RODATA;
    g->linkage = IR_LINK_LOCAL;
    g->relocs = NULL;
    vec_push(mod->globals, g);
    return idx;
}

/* Apply an InitPlan at `base`: zero-fill the object, then each write as a
   scalar OP_STORE or char-array OP_MEMCPY, GEP'd by the write's offset. */
static IrBlock *emit_init_plan(IrFunction *f, IrBlock *bb, IrOperand base, InitPlan *plan,
                               FuncBuilder *ctx)
{
    if (plan->total_size > 0)
    {
        u32 blob = ir_add_zero_blob(ctx->mod, ctx->mod->arena, (u32) plan->total_size);
        ir_emit_memcpy(bb, base, ir_operand_global(blob), (u32) plan->total_size);
    }
    if (!plan->writes)
    {
        return bb;
    }
    size_t n = vec_size(plan->writes);
    for (size_t i = 0; i < n; i++)
    {
        InitWrite *w = (InitWrite *) vec_get(plan->writes, i);
        u32 addr = alloc_vreg_from_type(ctx, type_ptr(w->type));
        ir_emit_gep(bb, addr, base, ir_operand_imm((i64) w->offset), 1);
        if (w->is_string_fill)
        {
            ASTStringLiteral *sl = ast_as(ASTStringLiteral, w->value);
            u32 sidx = ir_add_string_global(sl, ctx->mod, ctx->mod->arena);
            ir_emit_memcpy(bb, ir_operand_vreg(addr), ir_operand_global(sidx),
                           clamped_string_len(sl, w->type));
        }
        else
        {
            ExprResult val = build_expr(w->value, f, bb, ctx);
            bb = val.block;
            IrOperand o = promote_to(ctx, bb, val.value, node_type(w->value), w->type);
            ir_emit_store(bb, o, ir_operand_vreg(addr), w->type->size);
        }
    }
    return bb;
}

static ExprResult build_string_literal_expr(ASTStringLiteral *sl, IrFunction *f, IrBlock *bb,
                                            FuncBuilder *ctx)
{
    (void) f;
    u32 idx = ir_add_string_global(sl, ctx->mod, ctx->mod->arena);
    return expr_result(ir_operand_global(idx), bb);
}

/* Member address: both `.` and `->` GEP the object pointer by field_offset. */
static ExprResult build_member_lvalue(ASTMemberAccess *ma, IrFunction *f, IrBlock *bb,
                                      FuncBuilder *ctx)
{
    ExprResult obj = build_expr(ma->object, f, bb, ctx);
    bb = obj.block;
    u32 addr = alloc_vreg_from_type(ctx, type_ptr(ma->field_type));
    ir_emit_gep(bb, addr, obj.value, ir_operand_imm(1), ma->field_offset);
    return expr_result(ir_operand_vreg(addr), bb);
}

static ExprResult build_member_access_expr(ASTMemberAccess *ma, IrFunction *f, IrBlock *bb,
                                           FuncBuilder *ctx)
{
    ExprResult lv = build_member_lvalue(ma, f, bb, ctx);
    if (type_is_memory(ma->field_type))
    {
        return lv;
    }
    if (ma->field_type->kind == TYPE_VOID)
    {
        /* A `void`-typed field has no loading width; cf. build_deref_expr. */
        return expr_result(ir_operand_imm(0), lv.block);
    }
    u32 dst = alloc_vreg_from_type(ctx, ma->field_type);
    ir_emit_load(lv.block, dst, lv.value);
    return expr_result(ir_operand_vreg(dst), lv.block);
}

static ExprResult build_binary_expr(ASTBinaryExpr *be, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (plain_op(be->op) != be->op)
    {
        /* Not its own plain form: a compound assignment. */
        return build_compound_assign(be, f, bb, ctx);
    }
    switch (be->op)
    {
        case BIN_COMMA:
        {
            /* §6.5.17: evaluate the left and discard it, return the right. */
            ExprResult left = build_expr(be->left, f, bb, ctx);
            return build_expr(be->right, f, left.block, ctx);
        }
        case BIN_LOG_AND:
        case BIN_LOG_OR:
            return build_short_circuit(be, f, bb, ctx);
        case BIN_ASSIGN:
            return build_assign_expr(be, f, bb, ctx);
        default:
            return build_arith_binop_expr(be, f, bb, ctx);
    }
}

static ExprResult build_expr(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            return build_int_literal_expr(ast_as(ASTIntLiteral, node), f, bb, ctx);
        case AST_IDENT:
            return build_ident_expr(ast_as(ASTIdent, node), f, bb, ctx);
        case AST_BINARY_EXPR:
            return build_binary_expr(ast_as(ASTBinaryExpr, node), f, bb, ctx);
        case AST_UNARY_EXPR:
            return build_unary_expr(ast_as(ASTUnaryExpr, node), f, bb, ctx);
        case AST_INCDEC_EXPR:
            return build_incdec_expr(ast_as(ASTIncDecExpr, node), f, bb, ctx);
        case AST_CALL_EXPR:
            return build_call_expr(ast_as(ASTCallExpr, node), f, bb, ctx);
        case AST_TERNARY_EXPR:
            return build_ternary_expr(ast_as(ASTTernaryExpr, node), f, bb, ctx);
        case AST_SUBSCRIPT_EXPR:
            return build_subscript_expr(ast_as(ASTSubscriptExpr, node), f, bb, ctx);
        case AST_SIZEOF_EXPR:
        case AST_SIZEOF_TYPE:
        case AST_ALIGNOF_EXPR:
        case AST_ALIGNOF_TYPE:
            return build_size_const(node, f, bb, ctx);
        case AST_STRING_LITERAL:
            return build_string_literal_expr(ast_as(ASTStringLiteral, node), f, bb, ctx);
        case AST_MEMBER_ACCESS:
            return build_member_access_expr(ast_as(ASTMemberAccess, node), f, bb, ctx);
        case AST_CAST_EXPR:
            return build_cast_expr(ast_as(ASTCastExpr, node), f, bb, ctx);
        case AST_VA_ARG_EXPR:
            return build_va_arg_expr(ast_as(ASTVaArgExpr, node), f, bb, ctx);
        case AST_COMPOUND_LITERAL:
            return build_compound_literal_expr(ast_as(ASTCompoundLiteral, node), f, bb, ctx);
        default:
            ir_error(node, "unsupported expression kind %s", ast_kind_name(node->kind));
            return expr_result(ir_operand_imm(0), bb);
    }
}

static void collect_labels(ASTNode *node, IrFunction *f, FuncBuilder *ctx)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
        case AST_LABEL_STMT:
        {
            ASTLabelStmt *ls = ast_as(ASTLabelStmt, node);
            label_block(ctx, f, ls->label);
            collect_labels(ls->stmt, f, ctx);
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                ASTNode *stmt = (ASTNode *) vec_get(cs->stmts, i);
                collect_labels(stmt, f, ctx);
            }
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            collect_labels(is->then_branch, f, ctx);
            collect_labels(is->else_branch, f, ctx);
            break;
        }
        case AST_WHILE_STMT:
        {
            ASTWhileStmt *ws = ast_as(ASTWhileStmt, node);
            collect_labels(ws->body, f, ctx);
            break;
        }
        case AST_DO_WHILE_STMT:
        {
            ASTDoWhileStmt *ds = ast_as(ASTDoWhileStmt, node);
            collect_labels(ds->body, f, ctx);
            break;
        }
        case AST_FOR_STMT:
        {
            ASTForStmt *fs = ast_as(ASTForStmt, node);
            collect_labels(fs->body, f, ctx);
            break;
        }
        case AST_SWITCH_STMT:
        {
            ASTSwitchStmt *sw = ast_as(ASTSwitchStmt, node);
            collect_labels(sw->body, f, ctx);
            break;
        }
        case AST_CASE_STMT:
        {
            ASTCaseStmt *cs = ast_as(ASTCaseStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                collect_labels((ASTNode *) vec_get(cs->stmts, i), f, ctx);
            }
            break;
        }
        case AST_DEFAULT_STMT:
        {
            ASTDefaultStmt *ds = ast_as(ASTDefaultStmt, node);
            size_t n = vec_size(ds->stmts);
            for (size_t i = 0; i < n; i++)
            {
                collect_labels((ASTNode *) vec_get(ds->stmts, i), f, ctx);
            }
            break;
        }
        default:
            break;
    }
}

static IrBlock *label_block(FuncBuilder *ctx, IrFunction *f, const char *name)
{
    IrBlock *label_bb = (IrBlock *) strmap_get(ctx->goto_labels, name);
    if (!label_bb)
    {
        label_bb = ir_func_add_block(f, name);
        label_bb->is_loop_header = true;
        strmap_set(ctx->goto_labels, name, label_bb);
    }
    /* Label blocks are treated as merge points because backward gotos can add
       predecessor edges after this point, so PHIs must wait. */
    return label_bb;
}

static IrBlock *build_stmt_sequence(Vec *stmts, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    size_t n = vec_size(stmts);
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *stmt = (ASTNode *) vec_get(stmts, i);
        if (is_terminated(bb))
        {
            IrBlock *unreach = new_block(f, "unreach");
            bb = build_stmt(stmt, f, unreach, ctx);
        }
        else
        {
            bb = build_stmt(stmt, f, bb, ctx);
        }
    }
    return bb;
}

static IrBlock *build_compound_stmt(ASTCompoundStmt *cs, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    return build_stmt_sequence(cs->stmts, f, bb, ctx);
}

static void build_cond_branch(ASTNode *branch_stmt, IrFunction *f, IrBlock *branch_bb,
                              IrBlock *merge_bb, FuncBuilder *ctx)
{
    seal_block(ctx, branch_bb);

    IrBlock *end = branch_bb;
    if (branch_stmt)
    {
        end = build_stmt(branch_stmt, f, branch_bb, ctx);
    }
    if (!is_terminated(end))
    {
        jump(end, merge_bb);
    }
}

static IrBlock *build_if_stmt(ASTIfStmt *is, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult cond = build_expr(is->cond, f, bb, ctx);
    bb = cond.block;

    IrBlock *then_bb = new_block(f, "then");
    IrBlock *else_bb = new_block(f, "else");
    IrBlock *merge_bb = new_block(f, "merge");

    cond_jump(bb, cond.value, then_bb, else_bb);

    build_cond_branch(is->then_branch, f, then_bb, merge_bb, ctx);
    build_cond_branch(is->else_branch, f, else_bb, merge_bb, ctx);
    seal_block(ctx, merge_bb);
    return merge_bb;
}

static IrBlock *build_loop_body(ASTNode *body_node, IrFunction *f, LoopBlocks *lb, FuncBuilder *ctx)
{
    LoopContext lc = {.continue_target = lb->latch, .exit = lb->exit};
    vec_push(ctx->loop_stack, &lc);
    IrBlock *end = build_stmt(body_node, f, lb->body, ctx);
    vec_pop(ctx->loop_stack);
    return end;
}

static IrBlock *finish_loop(ASTNode *cond_node, IrFunction *f, LoopBlocks *lb, FuncBuilder *ctx)
{
    IrBlock *h = lb->header;
    if (cond_node)
    {
        ExprResult c = build_expr(cond_node, f, h, ctx);
        h = c.block;
        ir_emit_brcond(h, c.value, lb->body->label, lb->exit->label);
    }
    else
    {
        ir_emit_br(h, lb->body->label);
    }
    declare_pred(lb->exit, h);
    seal_block(ctx, h);
    seal_block(ctx, lb->exit);
    return lb->exit;
}

static void backedge(IrBlock *body_end, LoopBlocks *lb)
{
    if (!is_terminated(body_end))
    {
        jump(body_end, lb->latch);
    }
}

static IrBlock *build_while_stmt(ASTWhileStmt *ws, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    IrBlock *header_bb = new_block(f, "while_header");
    IrBlock *body_bb = new_block(f, "while_body");
    IrBlock *exit_bb = new_block(f, "while_exit");
    header_bb->is_loop_header = true;
    LoopBlocks lb = {header_bb, body_bb, header_bb, exit_bb};

    jump(bb, lb.header);
    declare_pred(lb.body, lb.header);
    backedge(build_loop_body(ws->body, f, &lb, ctx), &lb);

    return finish_loop(ws->cond, f, &lb, ctx);
}

static IrBlock *build_do_while_stmt(ASTDoWhileStmt *ds, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    IrBlock *body_bb = new_block(f, "do_body");
    IrBlock *header_bb = new_block(f, "do_header");
    IrBlock *exit_bb = new_block(f, "do_exit");
    header_bb->is_loop_header = true;
    LoopBlocks lb = {header_bb, body_bb, header_bb, exit_bb};

    jump(bb, lb.body);
    declare_pred(lb.body, lb.header);
    backedge(build_loop_body(ds->body, f, &lb, ctx), &lb);
    seal_block(ctx, lb.body);

    return finish_loop(ds->cond, f, &lb, ctx);
}

static IrBlock *build_for_stmt(ASTForStmt *fs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (fs->init)
    {
        bb = build_stmt(fs->init, f, bb, ctx);
    }

    IrBlock *header_bb = new_block(f, "for_header");
    IrBlock *body_bb = new_block(f, "for_body");
    IrBlock *latch_bb = new_block(f, "for_latch");
    IrBlock *exit_bb = new_block(f, "for_exit");
    header_bb->is_loop_header = true;
    LoopBlocks lb = {header_bb, body_bb, latch_bb, exit_bb};

    jump(bb, lb.header);
    declare_pred(lb.body, lb.header);
    backedge(build_loop_body(fs->body, f, &lb, ctx), &lb);

    /* Seal the latch before the post-expression so post reads see sealed
       predecessors. */
    seal_block(ctx, lb.latch);

    if (fs->post)
    {
        ExprResult post = build_expr(fs->post, f, lb.latch, ctx);
        lb.latch = post.block;
    }
    if (!is_terminated(lb.latch))
    {
        jump(lb.latch, lb.header);
    }

    return finish_loop(fs->cond, f, &lb, ctx);
}

static IrBlock *build_break_stmt(ASTBreakStmt *bs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) bs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    jump(bb, loop->exit);
    return bb;
}

static IrBlock *build_continue_stmt(ASTContinueStmt *cs, IrFunction *f, IrBlock *bb,
                                    FuncBuilder *ctx)
{
    (void) cs;
    (void) f;
    ASSERT(vec_size(ctx->loop_stack) > 0);
    LoopContext *loop = (LoopContext *) vec_last(ctx->loop_stack);
    jump(bb, loop->continue_target);
    return bb;
}

static IrBlock *build_goto_stmt(ASTGotoStmt *gs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    (void) f;
    IrBlock *target = (IrBlock *) strmap_get(ctx->goto_labels, gs->label);
    ASSERT(target != NULL);
    jump(bb, target);
    return bb;
}

static IrBlock *build_label_stmt(ASTLabelStmt *ls, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    IrBlock *label_bb = label_block(ctx, f, ls->label);
    if (!is_terminated(bb))
    {
        jump(bb, label_bb);
    }
    return build_stmt(ls->stmt, f, label_bb, ctx);
}

/* Collect case/default labels in source order, creating their blocks up front
   so the dispatch can target them. Nested switches are the inner builder's. */
static void collect_switch(ASTNode *node, IrFunction *f, FuncBuilder *ctx, SwitchCtx *sc,
                           Vec *cases)
{
    if (!node)
    {
        return;
    }
    switch (node->kind)
    {
        case AST_CASE_STMT:
        {
            ASTCaseStmt *cs = ast_as(ASTCaseStmt, node);
            SwitchCase *c = arena_alloc(ctx->mod->arena, sizeof(SwitchCase), sizeof(void *));
            c->value = cs->value;
            c->stmts = cs->stmts;
            c->bb = new_block(f, "switch_case");
            vec_push(cases, c);
            u64map_set(sc->case_blocks, (u64) cs->value, c->bb);
            /* Grouped labels (`case 1: case 2:`) arrive as nested ASTCaseStmt:
               register them so the body walk can resolve them. */
            size_t ns = vec_size(cs->stmts);
            for (size_t i = 0; i < ns; i++)
            {
                collect_switch((ASTNode *) vec_get(cs->stmts, i), f, ctx, sc, cases);
            }
            break;
        }
        case AST_DEFAULT_STMT:
        {
            ASTDefaultStmt *ds = ast_as(ASTDefaultStmt, node);
            sc->default_bb = new_block(f, "switch_default");
            sc->default_stmts = ds->stmts;
            size_t ns = vec_size(ds->stmts);
            for (size_t i = 0; i < ns; i++)
            {
                collect_switch((ASTNode *) vec_get(ds->stmts, i), f, ctx, sc, cases);
            }
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                collect_switch((ASTNode *) vec_get(cs->stmts, i), f, ctx, sc, cases);
            }
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            collect_switch(is->then_branch, f, ctx, sc, cases);
            collect_switch(is->else_branch, f, ctx, sc, cases);
            break;
        }
        case AST_WHILE_STMT:
            collect_switch(ast_as(ASTWhileStmt, node)->body, f, ctx, sc, cases);
            break;
        case AST_DO_WHILE_STMT:
            collect_switch(ast_as(ASTDoWhileStmt, node)->body, f, ctx, sc, cases);
            break;
        case AST_FOR_STMT:
            collect_switch(ast_as(ASTForStmt, node)->body, f, ctx, sc, cases);
            break;
        case AST_LABEL_STMT:
            collect_switch(ast_as(ASTLabelStmt, node)->stmt, f, ctx, sc, cases);
            break;
        default:
            break;
    }
}

static IrBlock *build_switch_stmt(ASTSwitchStmt *ss, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult cond = build_expr(ss->cond, f, bb, ctx);
    bb = cond.block;

    Type *cond_type = node_type(ss->cond);
    Type *promoted = type_promote(cond_type);
    IrOperand cv = promote_to(ctx, bb, cond.value, cond_type, promoted);

    SwitchCtx sc = {.case_blocks = u64map_new(ctx->mod->arena),
                    .exit_bb = new_block(f, "switch_exit")};

    Vec *cases = vec_new(ctx->mod->arena);
    collect_switch(ss->body, f, ctx, &sc, cases);

    /* One OP_SWITCH carries every case; the backend picks table vs chain, and the
       dispatch block preds every case/exit block. */
    size_t n = vec_size(cases);
    IrSwitchCase *sw_cases =
        arena_alloc(ctx->mod->arena, n * sizeof(IrSwitchCase), _Alignof(IrSwitchCase));
    for (size_t i = 0; i < n; i++)
    {
        SwitchCase *c = (SwitchCase *) vec_get(cases, i);
        sw_cases[i].val = c->value;
        sw_cases[i].label = c->bb->label;
        declare_pred(c->bb, bb);
    }
    IrBlock *no_match = sc.default_bb ? sc.default_bb : sc.exit_bb;
    declare_pred(no_match, bb);
    ir_emit_switch(bb, cv, (u32) n, sw_cases, no_match->label);

    /* `break` goes to the switch exit; `continue` (invalid in a bare switch)
       propagates to the enclosing loop's latch. */
    LoopContext *outer_loop =
        vec_size(ctx->loop_stack) ? (LoopContext *) vec_last(ctx->loop_stack) : NULL;
    LoopContext lc = {.continue_target = outer_loop ? outer_loop->continue_target : NULL,
                      .exit = sc.exit_bb};
    vec_push(ctx->loop_stack, &lc);
    vec_push(ctx->switch_stack, &sc);

    /* Walk the body as ordinary statements; `case`/`default` labels land in their
           pre-created blocks, giving exact C fall-through semantics. */
    IrBlock *end = build_stmt(ss->body, f, bb, ctx);

    vec_pop(ctx->switch_stack);
    vec_pop(ctx->loop_stack);

    if (!is_terminated(end))
    {
        jump(end, sc.exit_bb);
    }
    seal_block(ctx, sc.exit_bb);
    return sc.exit_bb;
}

/* Fallback path only for case labels nested inside inner blocks of a switch
   body (the parser's normalize_case_groups handles the top level). */
static IrBlock *build_case_stmt(ASTCaseStmt *cs, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    SwitchCtx *sc = (SwitchCtx *) vec_last(ctx->switch_stack);
    IrBlock *case_bb = u64map_get(sc->case_blocks, (u64) cs->value);
    ASSERT(case_bb != NULL);
    if (!is_terminated(bb))
    {
        jump(bb, case_bb);
    }
    return build_stmt_sequence(cs->stmts, f, case_bb, ctx);
}

static IrBlock *build_default_stmt(ASTDefaultStmt *ds, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    SwitchCtx *sc = (SwitchCtx *) vec_last(ctx->switch_stack);
    ASSERT(sc->default_bb != NULL);
    if (!is_terminated(bb))
    {
        jump(bb, sc->default_bb);
    }
    return build_stmt_sequence(ds->stmts, f, sc->default_bb, ctx);
}

static IrBlock *build_return_stmt(ASTReturnStmt *ret, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (ret->expr)
    {
        ExprResult val = build_expr(ret->expr, f, bb, ctx);
        if (type_is_record(f->ret_type))
        {
            ir_emit_memcpy(val.block, ir_operand_vreg(ctx->sret_vreg), val.value,
                           f->ret_type->size);
            ir_emit_ret(val.block, ir_operand_vreg(ctx->sret_vreg));
            return val.block;
        }
        val.value = promote_to(ctx, val.block, val.value, node_type(ret->expr), f->ret_type);
        ir_emit_ret(val.block, val.value);
        return val.block;
    }
    ir_emit_ret_void(bb);
    return bb;
}

static IrBlock *build_var_decl_stmt(ASTVarDecl *vd, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    if (vd->storage == SC_STATIC)
    {
        /* Block-scope statics are file-backed: the loaded image holds their
           constant initializer, so runtime sees a no-op. */
        if (block_static_index(ctx, vd) == NO_VREG &&
            emit_block_static(vd, ctx->mod, ctx->mod->arena, ctx->static_map, ctx->global_map) ==
                NO_VREG)
        {
            ctx->failed = true;
        }
        return bb;
    }
    if (vd->storage == SC_EXTERN)
    {
        /* Reference the external entity: emit an undefined global once if no
           file-scope declaration preceded it. */
        if (global_index_of(ctx, vd->name) == NO_VREG &&
            emit_global_decl(vd, ctx->mod, ctx->mod->arena, ctx->global_map, ctx->static_map) ==
                NO_VREG)
        {
            ctx->failed = true;
        }
        return bb;
    }

    if (type_is_memory(vd->type))
    {
        /* Memory objects: a stack slot whose pointer is the variable's value;
           plan writes zero-fill + store, an expression init applies via memcpy. */
        u32 dst = alloc_vreg_for_var(ctx, vd->type);
        ir_emit_alloca(bb, dst, vd->type->size);
        if (vd->plan)
        {
            bb = emit_init_plan(f, bb, ir_operand_vreg(dst), vd->plan, ctx);
        }
        else if (vd->init)
        {
            ExprResult init = build_expr(vd->init, f, bb, ctx);
            bb = init.block;
            ir_emit_memcpy(bb, ir_operand_vreg(dst), init.value, vd->type->size);
        }
        write_variable(ctx, vd, bb, ir_operand_vreg(dst));
        return bb;
    }

    IrOperand val = ir_operand_imm(0);
    if (vd->plan)
    {
        /* `{expr}` around a scalar unwraps to the single element. */
        if (vd->plan->writes && vec_size(vd->plan->writes) > 0)
        {
            InitWrite *w = (InitWrite *) vec_get(vd->plan->writes, 0);
            ExprResult init = build_expr(w->value, f, bb, ctx);
            bb = init.block;
            val = promote_to(ctx, bb, init.value, node_type(w->value), vd->type);
        }
    }
    else if (vd->init)
    {
        ExprResult init = build_expr(vd->init, f, bb, ctx);
        bb = init.block;
        val = promote_to(ctx, bb, init.value, node_type(vd->init), vd->type);
    }
    write_variable(ctx, vd, bb, val);
    return bb;
}

static IrBlock *build_expr_stmt(ASTExprStmt *es, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    ExprResult res = build_expr(es->expr, f, bb, ctx);
    return res.block;
}

static IrBlock *build_stmt(ASTNode *node, IrFunction *f, IrBlock *bb, FuncBuilder *ctx)
{
    switch (node->kind)
    {
        case AST_RETURN_STMT:
            return build_return_stmt(ast_as(ASTReturnStmt, node), f, bb, ctx);
        case AST_VAR_DECL:
            return build_var_decl_stmt(ast_as(ASTVarDecl, node), f, bb, ctx);
        case AST_DECL_LIST:
        {
            ASTDeclList *dl = ast_as(ASTDeclList, node);
            size_t n = vec_size(dl->decls);
            for (size_t i = 0; i < n; i++)
            {
                bb = build_var_decl_stmt(ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, i)), f,
                                         bb, ctx);
            }
            return bb;
        }
        case AST_STRUCT_DECL:
        case AST_ENUM_DECL:
        case AST_TYPEDEF_DECL:
            /* Tag/typedef definitions are parse-time only. */
            return bb;
        case AST_STATIC_ASSERT:
            /* Checked in the semantic pass; emits nothing. */
            return bb;
        case AST_EXPR_STMT:
            return build_expr_stmt(ast_as(ASTExprStmt, node), f, bb, ctx);
        case AST_COMPOUND_STMT:
            return build_compound_stmt(ast_as(ASTCompoundStmt, node), f, bb, ctx);
        case AST_IF_STMT:
            return build_if_stmt(ast_as(ASTIfStmt, node), f, bb, ctx);
        case AST_WHILE_STMT:
            return build_while_stmt(ast_as(ASTWhileStmt, node), f, bb, ctx);
        case AST_DO_WHILE_STMT:
            return build_do_while_stmt(ast_as(ASTDoWhileStmt, node), f, bb, ctx);
        case AST_FOR_STMT:
            return build_for_stmt(ast_as(ASTForStmt, node), f, bb, ctx);
        case AST_BREAK_STMT:
            return build_break_stmt(ast_as(ASTBreakStmt, node), f, bb, ctx);
        case AST_CONTINUE_STMT:
            return build_continue_stmt(ast_as(ASTContinueStmt, node), f, bb, ctx);
        case AST_GOTO_STMT:
            return build_goto_stmt(ast_as(ASTGotoStmt, node), f, bb, ctx);
        case AST_LABEL_STMT:
            return build_label_stmt(ast_as(ASTLabelStmt, node), f, bb, ctx);
        case AST_SWITCH_STMT:
            return build_switch_stmt(ast_as(ASTSwitchStmt, node), f, bb, ctx);
        case AST_CASE_STMT:
            return build_case_stmt(ast_as(ASTCaseStmt, node), f, bb, ctx);
        case AST_DEFAULT_STMT:
            return build_default_stmt(ast_as(ASTDefaultStmt, node), f, bb, ctx);
        default:
            ir_error(node, "unsupported statement kind %s", ast_kind_name(node->kind));
            return bb;
    }
}

static void setup_params(FuncBuilder *ctx, IrFunction *f, ASTFuncDef *ast, IrBlock *entry)
{
    f->is_variadic = ast->sig.is_variadic;
    ctx->sret_vreg = NO_VREG;
    if (type_is_record(ast->sig.ret_type))
    {
        /* Record returns arrive through a hidden sret pointer. */
        u32 vreg = alloc_vreg_for_var(ctx, ast->sig.ret_type);
        IrParam *p = arena_alloc(ctx->mod->arena, sizeof(IrParam), sizeof(void *));
        p->name = "__sret";
        p->type = type_ptr(ast->sig.ret_type);
        p->vreg = vreg;
        vec_push(f->params, p);
        ctx->sret_vreg = vreg;
    }

    size_t nparams = vec_size(ast->sig.params);
    for (size_t i = 0; i < nparams; i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(ast->sig.params, i));
        /* Record params arrive as a pointer to the caller's copy. */
        Type *ssa_type = var_ssa_type(param->type);
        u32 vreg = alloc_vreg_from_type(ctx, ssa_type);
        IrParam *p = arena_alloc(ctx->mod->arena, sizeof(IrParam), sizeof(void *));
        p->name = param->name;
        p->type = ssa_type;
        p->vreg = vreg;
        vec_push(f->params, p);
        write_variable(ctx, param, entry, ir_operand_vreg(vreg));
    }
}

static void seal_all_blocks(FuncBuilder *ctx, IrFunction *f)
{
    size_t nblocks = vec_size(f->blocks);
    for (size_t i = 0; i < nblocks; i++)
    {
        IrBlock *bb = (IrBlock *) vec_get(f->blocks, i);
        seal_block(ctx, bb);
    }
}

static void finish_func(IrFunction *f, IrBlock *exit, FuncBuilder *ctx)
{
    if (!is_terminated(exit))
    {
        if (f->ret_type->kind == TYPE_VOID)
        {
            ir_emit_ret_void(exit);
        }
        else
        {
            ir_emit_unreachable(exit);
        }
    }
    seal_all_blocks(ctx, f);
}

/* Pre-pass: record address-taken autos so their slot allocas exist before
   lowering (reads/writes go through memory before the `&` is visited). */
static void mark_addr_taken_expr(ASTNode *node, FuncBuilder *ctx);
static void mark_addr_taken_stmt(ASTNode *node, FuncBuilder *ctx);

static void spill_addr_ident(ASTVarDecl *decl, FuncBuilder *ctx)
{
    if (decl && is_spillable_var(decl) && !spill_slot(ctx, decl))
    {
        u64map_set(ctx->spill_slots, (u64) (uintptr_t) decl, (void *) 1);
        vec_push(ctx->spilled, decl);
    }
}

static void mark_addr_taken_expr(ASTNode *node, FuncBuilder *ctx)
{
    if (!node)
    {
        return;
    }
    switch (node->kind)
    {
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *ue = ast_as(ASTUnaryExpr, node);
            if (ue->op == UN_ADDR && ue->operand->kind == AST_IDENT)
            {
                spill_addr_ident(ast_as(ASTIdent, ue->operand)->decl, ctx);
            }
            mark_addr_taken_expr(ue->operand, ctx);
            break;
        }
        case AST_INCDEC_EXPR:
        {
            /* Descend: nested `&x` must still spill (`0[&c]`, `&*(&c)++`). */
            mark_addr_taken_expr(ast_as(ASTIncDecExpr, node)->operand, ctx);
            break;
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *be = ast_as(ASTBinaryExpr, node);
            mark_addr_taken_expr(be->left, ctx);
            mark_addr_taken_expr(be->right, ctx);
            break;
        }
        case AST_TERNARY_EXPR:
        {
            ASTTernaryExpr *te = ast_as(ASTTernaryExpr, node);
            mark_addr_taken_expr(te->cond, ctx);
            mark_addr_taken_expr(te->then_expr, ctx);
            mark_addr_taken_expr(te->else_expr, ctx);
            break;
        }
        case AST_CALL_EXPR:
        {
            ASTCallExpr *ce = ast_as(ASTCallExpr, node);
            size_t n = vec_size(ce->args);
            for (size_t i = 0; i < n; i++)
            {
                mark_addr_taken_expr((ASTNode *) vec_get(ce->args, i), ctx);
            }
            break;
        }
        case AST_SUBSCRIPT_EXPR:
        {
            ASTSubscriptExpr *se = ast_as(ASTSubscriptExpr, node);
            mark_addr_taken_expr(se->array, ctx);
            mark_addr_taken_expr(se->index, ctx);
            break;
        }
        case AST_SIZEOF_EXPR:
            mark_addr_taken_expr(ast_as(ASTSizeofExpr, node)->operand, ctx);
            break;
        case AST_MEMBER_ACCESS:
            mark_addr_taken_expr(ast_as(ASTMemberAccess, node)->object, ctx);
            break;
        case AST_CAST_EXPR:
            mark_addr_taken_expr(ast_as(ASTCastExpr, node)->operand, ctx);
            break;
        case AST_ALIGNOF_EXPR:
        case AST_ALIGNOF_TYPE:
            /* _Alignof does not evaluate its operand (§6.5.3.4p2). */
            break;
        case AST_COMPOUND_LITERAL:
            /* The literal's init list may carry `&x` — those autos must spill. */
            mark_addr_taken_expr(ast_as(ASTCompoundLiteral, node)->init, ctx);
            break;
        case AST_INIT_LIST:
        {
            ASTInitList *il = ast_as(ASTInitList, node);
            size_t n = vec_size(il->elems);
            for (size_t i = 0; i < n; i++)
            {
                InitElem *e = (InitElem *) vec_get(il->elems, i);
                mark_addr_taken_expr(e->value, ctx);
            }
            break;
        }
        default:
            break; /* literals, identifiers, sizeof-type: nothing to walk */
    }
}

static void mark_addr_taken_stmt(ASTNode *node, FuncBuilder *ctx)
{
    if (!node)
    {
        return;
    }
    switch (node->kind)
    {
        case AST_RETURN_STMT:
            mark_addr_taken_expr(ast_as(ASTReturnStmt, node)->expr, ctx);
            break;
        case AST_VAR_DECL:
            mark_addr_taken_expr(ast_as(ASTVarDecl, node)->init, ctx);
            break;
        case AST_EXPR_STMT:
            mark_addr_taken_expr(ast_as(ASTExprStmt, node)->expr, ctx);
            break;
        case AST_DECL_LIST:
        {
            ASTDeclList *dl = ast_as(ASTDeclList, node);
            size_t n = vec_size(dl->decls);
            for (size_t i = 0; i < n; i++)
            {
                mark_addr_taken_expr(ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, i))->init,
                                     ctx);
            }
            break;
        }
        case AST_COMPOUND_STMT:
        {
            ASTCompoundStmt *cs = ast_as(ASTCompoundStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                mark_addr_taken_stmt((ASTNode *) vec_get(cs->stmts, i), ctx);
            }
            break;
        }
        case AST_IF_STMT:
        {
            ASTIfStmt *is = ast_as(ASTIfStmt, node);
            mark_addr_taken_expr(is->cond, ctx);
            mark_addr_taken_stmt(is->then_branch, ctx);
            mark_addr_taken_stmt(is->else_branch, ctx);
            break;
        }
        case AST_WHILE_STMT:
        {
            ASTWhileStmt *ws = ast_as(ASTWhileStmt, node);
            mark_addr_taken_expr(ws->cond, ctx);
            mark_addr_taken_stmt(ws->body, ctx);
            break;
        }
        case AST_DO_WHILE_STMT:
        {
            ASTDoWhileStmt *ds = ast_as(ASTDoWhileStmt, node);
            mark_addr_taken_stmt(ds->body, ctx);
            mark_addr_taken_expr(ds->cond, ctx);
            break;
        }
        case AST_FOR_STMT:
        {
            ASTForStmt *fs = ast_as(ASTForStmt, node);
            mark_addr_taken_stmt(fs->init, ctx);
            mark_addr_taken_expr(fs->cond, ctx);
            mark_addr_taken_expr(fs->post, ctx);
            mark_addr_taken_stmt(fs->body, ctx);
            break;
        }
        case AST_LABEL_STMT:
            mark_addr_taken_stmt(ast_as(ASTLabelStmt, node)->stmt, ctx);
            break;
        case AST_SWITCH_STMT:
        {
            ASTSwitchStmt *sw = ast_as(ASTSwitchStmt, node);
            mark_addr_taken_expr(sw->cond, ctx);
            mark_addr_taken_stmt(sw->body, ctx);
            break;
        }
        case AST_CASE_STMT:
        {
            ASTCaseStmt *cs = ast_as(ASTCaseStmt, node);
            size_t n = vec_size(cs->stmts);
            for (size_t i = 0; i < n; i++)
            {
                mark_addr_taken_stmt((ASTNode *) vec_get(cs->stmts, i), ctx);
            }
            break;
        }
        case AST_DEFAULT_STMT:
        {
            ASTDefaultStmt *ds = ast_as(ASTDefaultStmt, node);
            size_t n = vec_size(ds->stmts);
            for (size_t i = 0; i < n; i++)
            {
                mark_addr_taken_stmt((ASTNode *) vec_get(ds->stmts, i), ctx);
            }
            break;
        }
        case AST_STATIC_ASSERT:
            /* Compile-time only (§6.7.4): nothing inside is address-taken. */
            break;
        default:
            break; /* break/continue/goto: no subexpressions */
    }
}

static bool build_func(ASTNode *ast, IrModule *mod, StrMap *func_types, StrMap *global_map)
{
    if (ast->kind != AST_FUNC_DEF)
    {
        ir_error(ast, "expected function definition at top level");
        return false;
    }
    ASTFuncDef *func_ast = (ASTFuncDef *) ast;

    IrFunction *func = ir_module_add_func(mod, func_ast->sig.name, func_ast->sig.ret_type);
    func->is_static = func_ast->sig.storage == SC_STATIC;
    IrBlock *entry = ir_func_add_block(func, "entry");

    if (func_ast->body->kind != AST_COMPOUND_STMT)
    {
        ir_error(func_ast->body, "expected compound statement as function body");
        return false;
    }

    FuncBuilder ctx = {.mod = mod,
                       .block_locals = u64map_new(mod->arena),
                       .loop_stack = vec_new(mod->arena),
                       .switch_stack = vec_new(mod->arena),
                       .goto_labels = strmap_new(mod->arena),
                       .func_types = func_types,
                       .global_map = global_map,
                       .static_map = u64map_new(mod->arena),
                       .spilled = vec_new(mod->arena),
                       .spill_slots = u64map_new(mod->arena),
                       .sret_vreg = NO_VREG};

    /* Pre-create blocks for all labels so gotos can target them. */
    collect_labels(func_ast->body, func, &ctx);

    /* Collect address-taken autos, hoist their slot allocas into the entry block,
           then wire params (address-taken params store into the existing slots). */
    mark_addr_taken_stmt(func_ast->body, &ctx);
    emit_spill_allocas(&ctx, entry);

    setup_params(&ctx, func, func_ast, entry);

    ASTCompoundStmt *body = (ASTCompoundStmt *) func_ast->body;
    entry = build_stmt_sequence(body->stmts, func, entry, &ctx);

    finish_func(func, entry, &ctx);

    return !ctx.failed;
}

IrModule *ir_build_module(ASTNode *ast, Arena *arena)
{
    if (ast->kind != AST_PROGRAM)
    {
        ir_error(ast, "expected program at top level");
        return NULL;
    }
    ASTProgram *prog = (ASTProgram *) ast;

    StrMap *func_types = strmap_new(arena);
    size_t ndecls = vec_size(prog->decls);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        Type *func_type = NULL;
        const char *fname = NULL;
        if (decl->kind == AST_FUNC_DEF)
        {
            ASTFuncDef *fn = ast_as(ASTFuncDef, decl);
            func_type = fn->sig.func_type;
            fname = fn->sig.name;
        }
        else if (decl->kind == AST_FUNC_DECL)
        {
            /* A prototype also seeds func_types so calls resolve; the ELF side
               makes the SHN_UNDEF symbol. */
            ASTFuncDecl *fd = ast_as(ASTFuncDecl, decl);
            func_type = fd->sig.func_type;
            fname = fd->sig.name;
        }
        if (func_type && fname)
        {
            strmap_set(func_types, fname, func_type);
        }
    }

    IrModule *mod = ir_module_new(arena);
    StrMap *global_map = strmap_new(arena);
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_VAR_DECL)
        {
            ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
            if (emit_global_decl(vd, mod, arena, global_map, NULL) == NO_VREG)
            {
                return NULL;
            }
        }
        else if (decl->kind == AST_DECL_LIST)
        {
            /* An init-declarator list at file scope: one global per
               declarator. */
            ASTDeclList *dl = ast_as(ASTDeclList, decl);
            size_t n = vec_size(dl->decls);
            for (size_t j = 0; j < n; j++)
            {
                ASTVarDecl *vd = ast_as(ASTVarDecl, (ASTNode *) vec_get(dl->decls, j));
                if (emit_global_decl(vd, mod, arena, global_map, NULL) == NO_VREG)
                {
                    return NULL;
                }
            }
        }
    }
    for (size_t i = 0; i < ndecls; i++)
    {
        ASTNode *decl = (ASTNode *) vec_get(prog->decls, i);
        if (decl->kind == AST_STRUCT_DECL || decl->kind == AST_ENUM_DECL ||
            decl->kind == AST_VAR_DECL || decl->kind == AST_TYPEDEF_DECL ||
            decl->kind == AST_DECL_LIST || decl->kind == AST_STATIC_ASSERT ||
            decl->kind == AST_FUNC_DECL)
        {
            continue;
        }
        if (!build_func(decl, mod, func_types, global_map))
        {
            return NULL;
        }
    }
    return mod;
}
