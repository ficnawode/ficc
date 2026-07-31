#ifndef FICC_DRIVER_H
#define FICC_DRIVER_H

#include "util/types.h"

/* Command-line flags. Expandable: add fields here, add parsing in driver.c. */
typedef struct {
    bool dump_tokens;
    bool dump_ast;
    bool dump_ir;
    bool emit_asm;
    bool emit_obj;
    bool run_interp;
} DriverFlags;

/* Parsed command-line arguments. */
typedef struct {
    const char *input_file;
    DriverFlags flags;
} DriverArgs;

#endif
