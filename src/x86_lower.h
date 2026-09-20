#ifndef FICC_X86_LOWER_H
#define FICC_X86_LOWER_H

#include "codegen.h"
#include "ir.h"
#include "util/arena.h"
#include "util/types.h"

/* Register-allocating lowering entry: fills cm->funcs from the IR module. */
size_t x86_lower_module(CodegenModule *cm, IrModule *ir, bool debug, Arena *arena);

#endif
