#ifndef FICC_PP_H
#define FICC_PP_H

#include "cli.h"
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
    bool system_header;
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
    bool is_pragma;
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
    PPConfig cfg;
    const char *exe_path;
    const char *builtin_dir;
    const char *cooked_date;
    const char *cooked_time;
    HashSet *pragma_once;
    HashSet *poison;
    bool has_source_date_epoch;
    i64 source_date_epoch;
    u32 error_count;
    u32 warning_count;
};

Pp *pp_new(Arena *arena);

void pp_free(Pp *pp);

Vec *pp_preprocess(Pp *pp, const char *file, const char *src);

void pp_apply_config(Pp *pp, const PPConfig *cfg);

void pp_define_cmdline(Pp *pp, const char *spec);
void pp_undef_cmdline(Pp *pp, const char *name);
void pp_include_cmdline(Pp *pp, const char *file);

void pp_error(Pp *pp, Loc loc, const char *fmt, ...);
void pp_warn(Pp *pp, Loc loc, const char *fmt, ...);
void pp_note(Loc loc, const char *fmt, ...);
void pp_verror(Pp *pp, Loc loc, const char *fmt, va_list args);
void pp_vwarn(Pp *pp, Loc loc, const char *fmt, va_list args);

void pp_dump(const Vec *soup);

#endif
