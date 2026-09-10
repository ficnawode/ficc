#ifndef FICC_PP_H
#define FICC_PP_H

#include "pp_lex.h"
#include "util/arena.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct PpIncludeFrame PpIncludeFrame;
struct PpIncludeFrame
{
    const char *file;
    Vec *tokens;
    size_t cursor;
};

typedef enum
{
    MACRO_OBJ,
    MACRO_FUNC,
} MacroKind;

typedef struct Macro Macro;
struct Macro
{
    const char *name;
    MacroKind kind;
    Loc loc;
    Vec *body;
    u32 param_count;
    bool variadic;
};

typedef struct Pp Pp;
struct Pp
{
    Arena *arena;
    Vec *out;
    Vec *includes;
    StrMap *macros;
    Vec *conds;
    Loc physical;
    Loc presumed;
    bool skipping;
    bool pedantic;
    Vec *include_paths;
    bool nostdinc;
    bool has_source_date_epoch;
    i64 source_date_epoch;
    u32 error_count;
    u32 warning_count;
};

/* Returns a new arena-allocated pp context with empty state. */
Pp *pp_new(Arena *arena);

/* Releases nothing; the arena owns all pp state. */
void pp_free(Pp *pp);

/* Runs translation phase 4 over src. Returns the output soup (also stored as
   pp->out), or NULL after reporting a diagnostic. */
Vec *pp_preprocess(Pp *pp, const char *file, const char *src);

/* Prints one line per soup token: presumed loc, kind, spelling, has_newline. */
void pp_dump(const Vec *soup);

#endif
