#include "cli.h"
#include "codegen.h"
#include "elf.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "pp.h"
#include "pp_emit.h"
#include "semantic.h"
#include "util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_stream(FILE *f, const char *what, Arena *arena)
{
    (void) what;
    size_t cap = 8192;
    size_t size = 0;
    char *buf = arena_alloc(arena, cap, 1);
    if (!buf)
    {
        return NULL;
    }
    for (;;)
    {
        if (size == cap)
        {
            cap *= 2;
            char *nb = arena_alloc(arena, cap, 1);
            if (!nb)
            {
                return NULL;
            }
            memcpy(nb, buf, size);
            buf = nb;
        }
        size_t n = fread(buf + size, 1, cap - size, f);
        if (n == 0)
        {
            break;
        }
        size += n;
    }
    buf[size] = '\0';
    return buf;
}

static char *read_file(const char *path, Arena *arena)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        return NULL;
    }
    char *buf = read_stream(f, path, arena);
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

static Pp *pp_from_config(const CompilerConfig *cfg)
{
    Pp *pp = pp_new(arena_new());
    pp->exe_path = cfg->exe_path;
    pp_apply_config(pp, &cfg->pp);
    return pp;
}

static int run_pipeline(const CompilerConfig *cfg, const char *input, Arena *arena, char *src)
{
    type_reset();

    Pp *pp = pp_from_config(cfg);
    Vec *soup = pp_preprocess(pp, input, src);
    if (!soup)
    {
        pp_free(pp);
        fprintf(stderr, "preprocess failed\n");
        return 1;
    }

    if (cfg->emit_pp || cfg->dump_pp)
    {
        if (cfg->emit_pp)
        {
            pp_emit(pp, stdout,
                    (PpEmitOptions) {.keep_comments = cfg->pp.keep_comments,
                                     .no_markers = cfg->pp.no_markers});
        }
        if (cfg->dump_pp)
        {
            pp_dump(soup);
        }
        pp_free(pp);
        return 0;
    }

    LexResult lexed = lex_finalize(soup, arena);
    pp_free(pp);
    if (!lexed.tokens)
    {
        fprintf(stderr, "lex failed\n");
        return 1;
    }
    if (cfg->dump_tokens)
    {
        for (size_t i = 0; i < lexed.count; i++)
        {
            Token *t = &lexed.tokens[i];
            printf("%s:%u:%u %s", t->loc.file, t->loc.line, t->loc.col, token_kind_name(t->kind));
            if (t->kind == TOK_INT_LIT)
            {
                printf(" %lld", (long long) t->payload.int_val);
            }
            else if (t->kind == TOK_FLOAT_LIT)
            {
                printf(" 0x%08llx%c", (unsigned long long) t->payload.float_pat,
                       t->float_kind == FK_FLOAT ? 'f' : '\0');
            }
            else if (t->kind == TOK_IDENT)
            {
                printf(" %s", t->payload.str);
            }
            printf("\n");
        }
    }

    ASTNode *ast = parse(lexed.tokens, lexed.count, &cfg->parser, arena);
    if (!ast)
    {
        fprintf(stderr, "parse failed\n");
        return 1;
    }
    if (cfg->dump_ast)
    {
        ast_dump(ast);
    }

    ast = semantic_check(ast, &cfg->semantic, arena);
    if (!ast)
    {
        fprintf(stderr, "semantic check failed\n");
        return 1;
    }

    IrModule *mod = ir_build_module(ast, &cfg->ir, arena);
    if (!mod)
    {
        fprintf(stderr, "IR build failed\n");
        return 1;
    }
    if (cfg->dump_ir)
    {
        ir_dump(mod);
    }

    if (cfg->run_interp)
    {
        i64 result = ir_interp_run(mod);
        printf("interp: %lld\n", (long long) result);
    }

    if (cfg->emit_obj)
    {
        CodegenModule *cm = codegen_ir_to_machine(mod, &cfg->codegen, arena);
        char outpath[256];
        if (cfg->output_path)
        {
            strncpy(outpath, cfg->output_path, sizeof(outpath) - 1);
            outpath[sizeof(outpath) - 1] = '\0';
        }
        else
        {
            replace_ext(input, outpath, sizeof(outpath), ".o");
        }
        elf_write(cm, outpath);
    }

    return 0;
}

int main(int argc, char **argv)
{
    Arena *arena = arena_new();
    CompilerConfig *cfg = cli_parse(argc, argv, arena);
    if (!cfg)
    {
        arena_free(arena);
        return 1;
    }
    if (cfg->show_help)
    {
        cli_help(cfg);
        arena_free(arena);
        return 0;
    }

    int rc = 0;
    for (size_t i = 0; i < vec_size(cfg->inputs) && rc == 0; i++)
    {
        const char *input = (const char *) vec_get(cfg->inputs, i);
        char *src;
        if (strcmp(input, "-") == 0)
        {
            src = read_stream(stdin, "stdin", arena);
        }
        else
        {
            src = read_file(input, arena);
        }
        if (!src)
        {
            rc = 1;
            break;
        }
        rc = run_pipeline(cfg, input, arena, src);
    }

    arena_free(arena);
    return rc;
}