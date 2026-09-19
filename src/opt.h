#ifndef FICC_OPT_H
#define FICC_OPT_H

#include "cli.h"
#include "ir.h"
#include "util/arena.h"

/* Sole public entry point: run the passes the level enables (none at -O0). */
void optimize(IrModule *mod, OptLevel level, Arena *arena);

#endif