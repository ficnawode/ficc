#include "codegen.h"
#include "elf.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "util/arena.h"
#include "util/sbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    bool dump_tokens;
    bool dump_ast;
    bool dump_ir;
    bool emit_asm;
    bool emit_obj;
    bool run_interp;
} DriverFlags;

typedef struct
{
    const char *input_file;
    DriverFlags flags;
} DriverArgs;

static char *read_file(const char *path, Arena *arena)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        perror(path);
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0)
    {
        perror(path);
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        perror(path);
        fclose(f);
        return NULL;
    }

    char *buf = arena_alloc(arena, (size_t) size + 1, 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t) size, f);
    if (nread != (size_t) size)
    {
        fprintf(stderr, "%s: short read (%zu of %ld bytes)\n", path, nread, size);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static void replace_ext(const char *in, char *out, size_t out_len, const char *new_ext)
{
    const char *dot = strrchr(in, '.');
    size_t base_len = dot ? (size_t) (dot - in) : strlen(in);
    size_t ext_len = strlen(new_ext);
    if (base_len + ext_len + 1 > out_len)
    {
        out[0] = '\0';
        return;
    }
    memcpy(out, in, base_len);
    memcpy(out + base_len, new_ext, ext_len + 1);
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-tokens] [-ast] [-ir] [-S] [-c] [-run] <file.c>\n", prog);
}

static bool parse_args(int argc, char **argv, DriverArgs *out)
{
    out->input_file = NULL;
    out->flags = (DriverFlags) {0};

    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        if (strcmp(arg, "-tokens") == 0)
        {
            out->flags.dump_tokens = true;
        }
        else if (strcmp(arg, "-ast") == 0)
        {
            out->flags.dump_ast = true;
        }
        else if (strcmp(arg, "-ir") == 0)
        {
            out->flags.dump_ir = true;
        }
        else if (strcmp(arg, "-S") == 0)
        {
            out->flags.emit_asm = true;
        }
        else if (strcmp(arg, "-c") == 0)
        {
            out->flags.emit_obj = true;
        }
        else if (strcmp(arg, "-run") == 0)
        {
            out->flags.run_interp = true;
        }
        else if (arg[0] == '-')
        {
            fprintf(stderr, "unknown flag: %s\n", arg);
            usage(argv[0]);
            return false;
        }
        else
        {
            if (out->input_file)
            {
                fprintf(stderr, "multiple input files not supported\n");
                usage(argv[0]);
                return false;
            }
            out->input_file = arg;
        }
    }

    if (!out->input_file)
    {
        usage(argv[0]);
        return false;
    }
    return true;
}

static int run_pipeline(const DriverArgs *args, Arena *arena, char *src)
{
    u64 tok_count;
    Token *tokens = lex(args->input_file, src, arena, &tok_count);
    if (!tokens)
    {
        fprintf(stderr, "lex failed\n");
        return 1;
    }
    if (args->flags.dump_tokens)
    {
        for (size_t i = 0; i < tok_count; i++)
        {
            Token *t = &tokens[i];
            printf("%s:%u:%u %s", t->loc.file, t->loc.line, t->loc.col, token_kind_name(t->kind));
            if (t->kind == TOK_INT_LIT)
                printf(" %lld", (long long) t->payload.int_val);
            else if (t->kind == TOK_IDENT)
                printf(" %s", t->payload.str);
            printf("\n");
        }
    }

    ASTNode *ast = parse(tokens, tok_count, arena);
    if (!ast)
    {
        fprintf(stderr, "parse failed\n");
        return 1;
    }
    if (args->flags.dump_ast)
    {
        ast_dump(ast);
    }

    ast = semantic_check(ast, arena);
    if (!ast)
    {
        fprintf(stderr, "semantic check failed\n");
        return 1;
    }

    Module *mod = ir_build_module(ast, arena);
    if (!mod)
    {
        fprintf(stderr, "IR build failed\n");
        return 1;
    }
    if (args->flags.dump_ir)
    {
        ir_dump(mod);
    }

    if (args->flags.run_interp)
    {
        i64 result = ir_interp_run(mod);
        printf("interp: %lld\n", (long long) result);
    }

    if (args->flags.emit_obj)
    {
        CodegenModule *cm = codegen_ir_to_machine(mod, arena);
        char outpath[256];
        replace_ext(args->input_file, outpath, sizeof(outpath), ".o");
        elf_write(cm, outpath);
    }

    if (args->flags.emit_asm)
    {
        CodegenModule *cm = codegen_ir_to_machine(mod, arena);
        Sbuf *sb = sbuf_new(arena);
        codegen_text_dump(cm, sb);
        printf("%s", sbuf_cstr(sb));
    }

    return 0;
}

int main(int argc, char **argv)
{
    DriverArgs args;
    if (!parse_args(argc, argv, &args))
        return 1;

    Arena *arena = arena_new();
    char *src = read_file(args.input_file, arena);
    if (!src)
        return 1;

    int rc = run_pipeline(&args, arena, src);
    arena_free(arena);
    return rc;
}
