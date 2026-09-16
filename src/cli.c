#include "cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef enum
{
    ARG_NONE,     /* -c, -E, -nostdinc: flag only */
    ARG_SEPARATE, /* value is the next argv item */
    ARG_JOINED,   /* value is glued to the name in the same item */
    ARG_EITHER,   /* -I dir or -Idir; -D name or -Dname; -o FILE or -o=FILE */
} ArgShape;

typedef struct
{
    const char *name;
    ArgShape shape;
    const char *value; /* help placeholder for the option's value; 0 for flags */
    bool (*apply)(CompilerConfig *cfg, const char *value);
    const char *help; /* one-line description for the aligned --help listing */
} CLIOption;

static void cli_config_init(CompilerConfig *cfg, Arena *arena, const char *exe_path)
{
    *cfg = (CompilerConfig) {0};
    cfg->arena = arena;
    cfg->exe_path = exe_path;
    cfg->inputs = vec_new(arena);
    cfg->pp.include_paths = vec_new(arena);
    cfg->pp.cmds = vec_new(arena);
}

static bool add_input(CompilerConfig *cfg, const char *path)
{
    vec_push(cfg->inputs, (void *) path);
    return true;
}

static bool add_include_path(CompilerConfig *cfg, const char *value)
{
    if (value[0] == '\0')
    {
        return false;
    }
    vec_push(cfg->pp.include_paths, (void *) value);
    return true;
}

static bool add_cmd(CompilerConfig *cfg, PPCommandKind kind, const char *arg)
{
    if (arg[0] == '\0')
    {
        return false;
    }
    PPCommand *cmd = arena_alloc(cfg->arena, sizeof(*cmd), sizeof(void *));
    cmd->kind = kind;
    cmd->arg = arg;
    vec_push(cfg->pp.cmds, cmd);
    return true;
}

static bool add_define(CompilerConfig *cfg, const char *value)
{
    return add_cmd(cfg, CMD_DEFINE, value);
}

static bool add_undef(CompilerConfig *cfg, const char *value)
{
    return add_cmd(cfg, CMD_UNDEF, value);
}

static bool add_include_file(CompilerConfig *cfg, const char *value)
{
    return add_cmd(cfg, CMD_INCLUDE, value);
}

static bool set_output_path(CompilerConfig *cfg, const char *value)
{
    if (value[0] == '\0')
    {
        return false;
    }
    cfg->output_path = value;
    return true;
}

static bool set_emit_pp(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->emit_pp = true;
    return true;
}

static bool set_emit_obj(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->emit_obj = true;
    return true;
}

static bool set_debug(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->codegen.debug = true;
    return true;
}

static bool set_run_interp(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->run_interp = true;
    return true;
}

static bool set_dump_tokens(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->dump_tokens = true;
    return true;
}

static bool set_dump_pp(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->dump_pp = true;
    return true;
}

static bool set_dump_ast(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->dump_ast = true;
    return true;
}

static bool set_ir_dump(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->dump_ir = true;
    return true;
}

static bool set_keep_comments(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->pp.keep_comments = true;
    cfg->emit_pp = true;
    return true;
}

static bool set_no_markers(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->pp.no_markers = true;
    cfg->emit_pp = true;
    return true;
}

static bool set_nostdinc(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->pp.nostdinc = true;
    return true;
}

static bool set_pedantic(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->pp.pedantic = true;
    cfg->parser.pedantic = true;
    cfg->semantic.pedantic = true;
    return true;
}

static bool clear_pedantic(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->pp.pedantic = false;
    cfg->parser.pedantic = false;
    cfg->semantic.pedantic = false;
    return true;
}

static bool set_help(CompilerConfig *cfg, const char *value)
{
    (void) value;
    cfg->show_help = true;
    return true;
}

static const CLIOption option_table[] = {
    {"--help", ARG_NONE, 0, set_help, "print the option listing and exit"},
    {"-E", ARG_NONE, 0, set_emit_pp, "print the preprocessed source to stdout"},
    {"-c", ARG_NONE, 0, set_emit_obj, "compile to an object file"},
    {"-g", ARG_NONE, 0, set_debug, "emit DWARF debug info and call-frame info"},
    {"-run", ARG_NONE, 0, set_run_interp, "execute the program in the interpreter"},
    {"-nostdinc", ARG_NONE, 0, set_nostdinc, "do not search the builtin include directory"},
    {"-tokens", ARG_NONE, 0, set_dump_tokens, "dump the token stream"},
    {"-pp", ARG_NONE, 0, set_dump_pp, "dump the preprocessor token soup"},
    {"-ast", ARG_NONE, 0, set_dump_ast, "dump the syntax tree"},
    {"-ir", ARG_NONE, 0, set_ir_dump, "dump the intermediate representation"},
    {"-C", ARG_NONE, 0, set_keep_comments, "keep comments when preprocessing (implies -E)"},
    {"-P", ARG_NONE, 0, set_no_markers, "omit #line markers when preprocessing (implies -E)"},
    {"-fpedantic", ARG_NONE, 0, set_pedantic, "enable pedantic diagnostics"},
    {"-fno-pedantic", ARG_NONE, 0, clear_pedantic, "disable pedantic diagnostics"},
    {"-I", ARG_EITHER, "dir", add_include_path, "add a directory to the include search path"},
    {"-D", ARG_EITHER, "name[=val]", add_define, "define a macro"},
    {"-U", ARG_EITHER, "name", add_undef, "undefine a macro"},
    {"-include", ARG_EITHER, "file", add_include_file, "include a file before the main input"},
    {"-o", ARG_EITHER, "file", set_output_path, "write the object to file (single input)"},
    {0},
};

#define USAGE_INDENT 7
#define USAGE_WRAP 78

/* Prints one bracketed token, wrapping to the hanging indent when it would
   overflow the usage line. Returns the new column. */
static int usage_print_token(int col, const char *token)
{
    if (col > USAGE_INDENT && col + 1 + (int) strlen(token) > USAGE_WRAP)
    {
        printf("\n");
        for (int i = 0; i < USAGE_INDENT; i++)
        {
            printf(" ");
        }
        printf("%s", token);
        return USAGE_INDENT + (int) strlen(token);
    }
    printf(" %s", token);
    return col + 1 + (int) strlen(token);
}

void cli_usage(const CompilerConfig *cfg)
{
    printf("Usage: %s", cfg->exe_path);
    int col = (int) strlen("Usage: ") + (int) strlen(cfg->exe_path);
    for (const CLIOption *o = option_table; o->name; o++)
    {
        char token[64];
        if (o->shape == ARG_NONE)
        {
            snprintf(token, sizeof(token), "[%s]", o->name);
        }
        else
        {
            snprintf(token, sizeof(token), "[%s %s]", o->name, o->value);
        }
        col = usage_print_token(col, token);
    }
    usage_print_token(col, "<file.c>...");
    printf("\n");
}

void cli_help(const CompilerConfig *cfg)
{
    cli_usage(cfg);
    printf("\nOptions:\n");
    for (const CLIOption *o = option_table; o->name; o++)
    {
        char token[64];
        if (o->shape == ARG_NONE)
        {
            snprintf(token, sizeof(token), "[%s]", o->name);
        }
        else
        {
            snprintf(token, sizeof(token), "[%s %s]", o->name, o->value);
        }
        printf("  %-18s %s\n", token, o->help);
    }
}

static CompilerConfig *err(CompilerConfig *cfg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void) cfg;
    printf("ficc: error: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    return NULL;
}

static CompilerConfig *bad(CompilerConfig *cfg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("ficc: error: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    cli_usage(cfg);
    return NULL;
}

static const CLIOption *lookup(const CLIOption *table, const char *arg, const char **name_out,
                               const char **suffix_out)
{
    for (const CLIOption *o = table; o->name; o++)
    {
        if (strcmp(o->name, arg) == 0)
        {
            *name_out = o->name;
            *suffix_out = "";
            return o;
        }
    }
    for (const CLIOption *o = table; o->name; o++)
    {
        if (o->shape != ARG_NONE && strncmp(arg, o->name, strlen(o->name)) == 0)
        {
            *name_out = o->name;
            *suffix_out = arg + strlen(o->name);
            return o;
        }
    }
    /* -f/-fno- pairing: -fno-X falls back to its -fX entry. Both pedantic
       spellings are listed explicitly, so this only fires for a future flag
       that lists a single -f form. */
    if (strncmp(arg, "-fno-", 5) == 0)
    {
        char set_name[64];
        size_t rest = strlen(arg + 5);
        if (rest + 3 <= sizeof(set_name))
        {
            memcpy(set_name, "-f", 2);
            memcpy(set_name + 2, arg + 5, rest + 1);
            return lookup(table, set_name, name_out, suffix_out);
        }
    }
    return NULL;
}

CompilerConfig *cli_parse(int argc, char **argv, Arena *arena)
{
    CompilerConfig *cfg = arena_alloc(arena, sizeof(*cfg), sizeof(void *));
    const char *exe = "ficc";
    if (argc > 0 && argv[0])
    {
        exe = argv[0];
    }
    cli_config_init(cfg, arena, exe);

    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0)
        {
            for (i++; i < argc; i++)
            {
                if (!add_input(cfg, argv[i]))
                {
                    return NULL;
                }
            }
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0' || strcmp(arg, "-") == 0)
        {
            if (!add_input(cfg, arg))
            {
                return NULL;
            }
            continue;
        }

        const char *name = NULL;
        const char *suffix = NULL;
        const CLIOption *spec = lookup(option_table, arg, &name, &suffix);
        if (!spec)
        {
            return bad(cfg, "unknown option: %s", arg);
        }

        const char *value = suffix;
        if (*value == '=')
        {
            value++;
        }
        if (spec->shape != ARG_NONE && *value == '\0' &&
            (spec->shape == ARG_SEPARATE || spec->shape == ARG_EITHER))
        {
            if (i + 1 >= argc)
            {
                return bad(cfg, "%s requires an argument", name);
            }
            value = argv[++i];
        }

        if (!spec->apply(cfg, value))
        {
            return bad(cfg, "bad value for %s: %s", name, value);
        }
        if (cfg->show_help)
        {
            break; /* --help: nothing after it matters */
        }
    }

    if (cfg->show_help)
    {
        return cfg; /* caller prints cli_help and exits 0 */
    }
    if (vec_size(cfg->inputs) == 0)
    {
        cli_usage(cfg);
        return NULL;
    }
    if (cfg->output_path && vec_size(cfg->inputs) != 1)
    {
        err(cfg, "-o requires exactly one input");
        cli_usage(cfg);
        return NULL;
    }
    return cfg;
}