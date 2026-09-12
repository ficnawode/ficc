#ifndef FICC_SEMANTIC_H
#define FICC_SEMANTIC_H

#include "ast.h"
#include "cli.h"

ASTNode *semantic_check(ASTNode *ast, const SemanticConfig *cfg, Arena *arena);

#endif
