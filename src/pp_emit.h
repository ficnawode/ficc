#ifndef FICC_PP_EMIT_H
#define FICC_PP_EMIT_H

#include "pp.h"
#include <stdio.h>

typedef struct
{
    bool keep_comments;
    bool no_markers;
} PpEmitOptions;

void pp_emit(Pp *pp, FILE *f, PpEmitOptions opts);

#endif