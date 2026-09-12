#ifndef FICC_IR_BUILDER_H
#define FICC_IR_BUILDER_H

#include "ast.h"
#include "cli.h"
#include "ir.h"

IrModule *ir_build_module(ASTNode *ast, const IRConfig *cfg, Arena *arena);

#endif
