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

static Function *find_main(Module *m)
{
    size_t n = vec_size(m->funcs);
    for (size_t i = 0; i < n; i++) {
        Function *f = (Function *)vec_get(m->funcs, i);
        if (strcmp(f->name, "main") == 0)
            return f;
    }
    return NULL;
}

i64 ir_interp_run(Module *m)
{
    Function *main_fn = find_main(m);
    if (!main_fn) {
        fprintf(stderr, "[interp] error: no main function found\n");
        return 1;
    }

    /* Module-wide dense register file, zero-initialized. */
    Arena *frame_arena = arena_new();
    u32 nregs = m->next_vreg;
    i64 *regs = arena_alloc(frame_arena, nregs * sizeof(i64), sizeof(i64));
    memset(regs, 0, nregs * sizeof(i64));

    Block *entry = (Block *)vec_get(main_fn->blocks, 0);
    size_t ninstr = vec_size(entry->instrs);
    i64 result = 0;

    for (size_t i = 0; i < ninstr; i++) {
        Instr *in = (Instr *)vec_get(entry->instrs, i);
        switch (in->opcode) {
        case OP_RET:
            if (in->nops > 0)
                result = operand_val(in->ops[0], regs, nregs);
            arena_free(frame_arena);
            return result;
        case OP_UNREACHABLE:
            fprintf(stderr, "[interp] error: reached unreachable\n");
            arena_free(frame_arena);
            return 1;
        default:
            fprintf(stderr, "[interp] error: unsupported opcode %s\n", ir_opcode_name(in->opcode));
            arena_free(frame_arena);
            return 1;
        }
    }

    arena_free(frame_arena);
    return result;
}
