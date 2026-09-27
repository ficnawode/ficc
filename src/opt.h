#ifndef FICC_OPT_H
#define FICC_OPT_H

#include "cli.h"
#include "ir.h"
#include "util/arena.h"

void optimize(IrModule *mod, OptLevel level, Arena *arena);

#endif