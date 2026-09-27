#ifndef FICC_PP_EXPR_H
#define FICC_PP_EXPR_H

#include "pp.h"

typedef struct
{
    u64 value;
    bool is_unsigned;
} PpExprVal;

PpExprVal pp_eval_expr(Pp *pp, const Vec *tokens);

#endif