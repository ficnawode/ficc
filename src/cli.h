#ifndef FICC_CLI_H
#define FICC_CLI_H

#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct
{
    bool keep_comments;        /* -C: print comment trivia (implies emit_pp) */
    bool no_markers;           /* -P: omit #line markers (implies emit_pp) */
    bool nostdinc;             /* -nostdinc */
    bool pedantic;             /* -fpedantic / -fno-pedantic */
    Vec *include_paths;        /* Vec<const char*> — -I dir, in order */
    Vec *system_include_paths; /* Vec<const char*> — -isystem dir, after -I */
    Vec *cmds;                 /* Vec<PPCommand*>: -D/-U/-include in command-line order */
} PPConfig;

typedef enum
{
    CMD_DEFINE,
    CMD_UNDEF,
    CMD_INCLUDE
} PPCommandKind;
typedef struct
{
    PPCommandKind kind;
    const char *arg;
} PPCommand;

typedef struct
{
    bool pedantic;
} ParserConfig;

/* Separate struct on purpose: separation of responsibility. Semantic gets its own
   sub-config even though it only needs `pedantic` today, so a growing set of semantic
   flags (mode errors, warnings-as-errors, strictness levels...) stays scoped here and
   does not leak into ParserConfig. */
typedef struct
{
    bool pedantic; /* shares spelling with ParserConfig.pedantic, set in tandem */
} SemanticConfig;

/* The -O levels; each expands inside the optimizer to its pass set (-O0 = none). */
typedef enum
{
    OPT_LEVEL_0 = 0,
    OPT_LEVEL_1,
    OPT_LEVEL_2,
    OPT_LEVEL_3,
} OptLevel;

/* IR / codegen fragments are empty placeholders for now (future -O, -march, -g);
   passing them now avoids signature churn later. */
typedef struct
{
} IRConfig;

typedef struct
{
    bool debug; /* -g: emit DWARF debug info + .eh_frame CFI in the object */
} CodegenConfig;

/* The link step's configuration. Append-only; the linker reads it through
   `link_run` and never sees the driver's own flags. */
typedef struct
{
    const char *output_path; /* -o FILE, or NULL for the default a.out */
    Vec *lib_paths;          /* Vec<const char*> — -L dir, in order */
    Vec *libs;               /* Vec<const char*> — -l name, in order */
    bool nostdlib;           /* -nostdlib: no implicit startup or -lc */
    bool static_;            /* -static (residual: dynamic is the hosted default) */
} LinkConfig;

typedef struct
{
    Arena *arena;            /* owns all string lifetimes for the parse */
    const char *exe_path;    /* argv[0] */
    const char *output_path; /* -o FILE — exact object for a single input; else derived */
    Vec *inputs;             /* Vec<const char*>: input files or "-" */

    /* ficc-specific driver/pipe flags (independent; never overwritten) */
    bool emit_pp;     /* -E */
    bool dump_pp;     /* -pp */
    bool dump_tokens; /* -tokens */
    bool dump_ast;    /* -ast */
    bool dump_ir;     /* -ir */
    bool emit_obj;    /* -c */
    bool run_interp;  /* -run */
    bool show_help;   /* --help: print the option listing and leave */

    /* sub-configs handed to each stage */
    PPConfig pp;
    ParserConfig parser;
    SemanticConfig semantic;
    IRConfig ir;
    OptLevel opt; /* the -O level the optimizer runs at */
    CodegenConfig codegen;
    LinkConfig link; /* the link step's config (drives link_run) */
} CompilerConfig;

CompilerConfig *cli_parse(int argc, char **argv, Arena *arena);
void cli_usage(const CompilerConfig *cfg);
void cli_help(const CompilerConfig *cfg);

#endif
