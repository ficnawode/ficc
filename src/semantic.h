#ifndef FICC_SEMANTIC_H
#define FICC_SEMANTIC_H

#include "ast.h"

ASTNode *semantic_check(ASTNode *ast, Arena *arena);

#endif
