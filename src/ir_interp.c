#include "ir_interp.h"
#include "util/assert.h"
#include <stdio.h>
#include <string.h>

static i64 operand_val(Operand o, i64 *regs, u32 nregs)
{
    if (o.is_imm)
        return o.u.imm;
    ASSERT(o.u.vreg < nregs);
    return regs[o.u.vreg];
}

static Function *find_func(Module *m, const char *name)
{
    size_t n = vec_size(m->funcs);
    for (size_t i = 0; i < n; i++) {
        Function *f = (Function *)vec_get(m->funcs, i);
        if (strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

typedef struct Frame Frame;
struct Frame {
    Function *func;
    i64 *regs;
};

static Frame *frame_new(Arena *arena, Function *func, u32 nregs)
{
    Frame *f = arena_alloc(arena, sizeof(Frame), sizeof(void *));
    f->func = func;
    f->regs = arena_alloc(arena, nregs * sizeof(i64), sizeof(i64));
    memset(f->regs, 0, nregs * sizeof(i64));
    return f;
}

static i64 exec_func(Module *m, Frame *fr, Vec *stack, Arena *frame_arena, u32 nregs);

static i64 run_func(Module *m, Function *func, Vec *stack, Arena *frame_arena, u32 nregs)
{
    Frame *fr = frame_new(frame_arena, func, nregs);
    vec_push(stack, fr);
    i64 result = exec_func(m, fr, stack, frame_arena, nregs);
    vec_pop(stack);
    return result;
}

static i64 exec_func(Module *m, Frame *fr, Vec *stack, Arena *frame_arena, u32 nregs)
{
    Block *entry = (Block *)vec_get(fr->func->blocks, 0);
    size_t ninstr = vec_size(entry->instrs);

    for (size_t i = 0; i < ninstr; i++) {
        Instr *in = (Instr *)vec_get(entry->instrs, i);
        switch (in->opcode) {
        case OP_ADD:
            fr->regs[in->result] = operand_val(in->ops[0], fr->regs, nregs) +
                                    operand_val(in->ops[1], fr->regs, nregs);
            break;
        case OP_SUB:
            fr->regs[in->result] = operand_val(in->ops[0], fr->regs, nregs) -
                                    operand_val(in->ops[1], fr->regs, nregs);
            break;
        case OP_MUL:
            fr->regs[in->result] = operand_val(in->ops[0], fr->regs, nregs) *
                                    operand_val(in->ops[1], fr->regs, nregs);
            break;
        case OP_SDIV: {
            i64 rhs = operand_val(in->ops[1], fr->regs, nregs);
            if (rhs == 0) {
                fprintf(stderr, "[interp] error: division by zero\n");
                ASSERT(false);
                return 1;
            }
            fr->regs[in->result] = operand_val(in->ops[0], fr->regs, nregs) / rhs;
            break;
        }
        case OP_SREM: {
            i64 rhs = operand_val(in->ops[1], fr->regs, nregs);
            if (rhs == 0) {
                fprintf(stderr, "[interp] error: division by zero\n");
                ASSERT(false);
                return 1;
            }
            fr->regs[in->result] = operand_val(in->ops[0], fr->regs, nregs) % rhs;
            break;
        }
        case OP_NEG:
            fr->regs[in->result] = -operand_val(in->ops[0], fr->regs, nregs);
            break;
        case OP_CALL: {
            Function *callee = find_func(m, in->extra.call.name);
            if (!callee) {
                fprintf(stderr, "[interp] error: undefined function '%s'\n", in->extra.call.name);
                return 1;
            }
            Frame *callee_fr = frame_new(frame_arena, callee, nregs);
            vec_push(stack, callee_fr);
            /* Copy args into callee param vregs */
            for (u32 a = 0; a < in->extra.call.nargs; a++) {
                Param *p = (Param *)vec_get(callee->params, a);
                callee_fr->regs[p->vreg] = operand_val(in->extra.call.args[a], fr->regs, nregs);
            }
            i64 ret = exec_func(m, callee_fr, stack, frame_arena, nregs);
            vec_pop(stack);
            fr->regs[in->result] = ret;
            break;
        }
        case OP_RET: {
            i64 result = 0;
            if (in->nops > 0)
                result = operand_val(in->ops[0], fr->regs, nregs);
            return result;
        }
        case OP_UNREACHABLE:
            fprintf(stderr, "[interp] error: reached unreachable\n");
            return 1;
        default:
            fprintf(stderr, "[interp] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
            ASSERT(false);
            return 1;
        }
    }

    return 0;
}

i64 ir_interp_run(Module *m)
{
    Function *main_fn = find_func(m, "main");
    if (!main_fn) {
        fprintf(stderr, "[interp] error: no main function found\n");
        return 1;
    }

    Arena *frame_arena = arena_new();
    Vec *stack = vec_new(frame_arena);
    u32 nregs = m->next_vreg;

    i64 result = run_func(m, main_fn, stack, frame_arena, nregs);

    arena_free(frame_arena);
    return result;
}
