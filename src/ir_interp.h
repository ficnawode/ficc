#ifndef FICC_IR_INTERP_H
#define FICC_IR_INTERP_H

#include "ir.h"
#include "util/types.h"

/* Execute the IR module, returning the exit code of `main`.
   For Phase 1, `main` has no arguments. */
i64 ir_interp_run(Module *m);

#endif
