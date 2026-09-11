#ifndef FICC_PP_EXPR_H
#define FICC_PP_EXPR_H

#include "pp.h"

typedef struct
{
    u64 value;
    bool is_unsigned;
} PpExprVal;

/* Evaluates an expanded #if controlling expression (D17.7). Reports
   diagnostics through pp_error and recovers. */
PpExprVal pp_eval_expr(Pp *pp, const Vec *tokens);

#endif