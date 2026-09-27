#ifndef FICC_CLI_H
#define FICC_CLI_H

#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct
{
    bool keep_comments;
    bool no_markers;
    bool nostdinc;
    bool pedantic;
    Vec *include_paths;
    Vec *system_include_paths;
    Vec *cmds;
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

typedef struct
{
    bool pedantic;
} SemanticConfig;

typedef enum
{
    OPT_LEVEL_0 = 0,
    OPT_LEVEL_1,
    OPT_LEVEL_2,
    OPT_LEVEL_3,
} OptLevel;

typedef struct
{
} IRConfig;

typedef struct
{
    bool debug;
} CodegenConfig;

typedef struct
{
    const char *output_path;
    Vec *lib_paths;
    Vec *libs;
    bool nostdlib;
    bool static_;
    bool export_dynamic;
} LinkConfig;

typedef struct
{
    Arena *arena; /* owns all string lifetimes for the parse */
    const char *exe_path;
    const char *output_path;
    Vec *inputs;

    bool emit_pp;
    bool dump_pp;
    bool dump_tokens;
    bool dump_ast;
    bool dump_ir;
    bool emit_obj;
    bool run_interp;
    bool show_help;

    PPConfig pp;
    ParserConfig parser;
    SemanticConfig semantic;
    IRConfig ir;
    OptLevel opt;
    CodegenConfig codegen;
    LinkConfig link;
} CompilerConfig;

CompilerConfig *cli_parse(int argc, char **argv, Arena *arena);
void cli_usage(const CompilerConfig *cfg);
void cli_help(const CompilerConfig *cfg);

#endif
