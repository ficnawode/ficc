#ifndef FICC_PP_H
#define FICC_PP_H

#include "pp_lex.h"
#include "util/arena.h"
#include "util/hashmap.h"
#include "util/types.h"
#include "util/vec.h"

#include <stdarg.h>

typedef struct PpIncludeFrame PpIncludeFrame;
struct PpIncludeFrame
{
    const char *file;
    Vec *tokens;
    size_t cursor;
    u32 presumed_line;
    const char *presumed_file;
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
    bool predefined;
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
    const char *exe_path;
    const char *builtin_dir;
    const char *cooked_date;
    const char *cooked_time;
    bool has_source_date_epoch;
    i64 source_date_epoch;
    u32 error_count;
    u32 warning_count;
};

/* Returns a new pp context backed by its own arena; pp_free releases it. */
Pp *pp_new(Arena *arena);

/* Releases the pp arena (normalized source, soup, macros). The pp output is
   only needed until lex_finalize has run. */
void pp_free(Pp *pp);

/* Runs translation phase 4 over src. Returns the output soup (also stored as
   pp->out), or NULL after reporting a diagnostic. */
Vec *pp_preprocess(Pp *pp, const char *file, const char *src);

/* Diagnostics; exported so the #if evaluator can report through the same sink. */
void pp_error(Pp *pp, Loc loc, const char *fmt, ...);
void pp_warn(Pp *pp, Loc loc, const char *fmt, ...);
void pp_note(Loc loc, const char *fmt, ...);
void pp_verror(Pp *pp, Loc loc, const char *fmt, va_list args);
void pp_vwarn(Pp *pp, Loc loc, const char *fmt, va_list args);

/* Prints one line per soup token: presumed loc, kind, spelling, has_newline. */
void pp_dump(const Vec *soup);

#endif
