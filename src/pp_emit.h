#ifndef FICC_PP_EMIT_H
#define FICC_PP_EMIT_H

#include "pp.h"
#include <stdio.h>

typedef struct
{
    bool keep_comments; /* -C: print comment trivia */
    bool no_markers;    /* -P: omit `# NN "file"` markers */
} PpEmitOptions;

/* Prints the phase-4 soup (pp->out) to f: each real token's spelling preceded
   by its trivia, comment trivia only with -C. `# NN "file"` pseudo-lines are
   emitted on the first line and whenever the presumed line/file jumps unless
   no_markers, so the output re-preprocesses to the identical soup (D17.13). */
void pp_emit(Pp *pp, FILE *f, PpEmitOptions opts);

#endif