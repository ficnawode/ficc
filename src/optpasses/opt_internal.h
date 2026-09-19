#ifndef FICC_OPT_INTERNAL_H
#define FICC_OPT_INTERNAL_H

/* Private opt surface for the passes and the unit tests. */
#include <stdbool.h>

/* Forward-declared: a quoted include would not resolve from this directory. */
struct IrModule;

bool opt_verify(struct IrModule *mod);

#endif