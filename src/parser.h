#ifndef FICC_PARSER_H
#define FICC_PARSER_H

#include "ast.h"

ASTNode *parse(Token *tokens, u64 count, Arena *arena);

#endif
