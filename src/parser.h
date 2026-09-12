#ifndef FICC_PARSER_H
#define FICC_PARSER_H

#include "ast.h"
#include "cli.h"

ASTNode *parse(Token *tokens, u64 count, const ParserConfig *cfg, Arena *arena);

#endif
