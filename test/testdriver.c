#include "testdriver.h"

#include "codegen.h"
#include "elf.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "optpasses/opt_internal.h"
#include "parser.h"
#include "pp.h"
#include "semantic.h"
#include "type.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int tc_run_shell(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
    {
        return -1;
    }
    return WEXITSTATUS(rc);
}

ASTNode *tc_parse(const char *src, Arena *arena)
{
    Pp *pp = pp_new(arena_new());
    Vec *soup = pp_preprocess(pp, "<test>", src);
    if (!soup)
    {
        pp_free(pp);
        return NULL;
    }
    LexResult lexed = lex_finalize(soup, arena);
    pp_free(pp);
    if (!lexed.tokens)
    {
        return NULL;
    }
    ParserConfig pc = {0};
    return parse(lexed.tokens, lexed.count, &pc, arena);
}

IrModule *tc_build_module(const char *src, Arena *arena)
{
    type_reset();
    ASTNode *ast = tc_parse(src, arena);
    if (!ast)
    {
        return NULL;
    }
    ast = semantic_check(ast, &(SemanticConfig) {0}, arena);
    if (!ast)
    {
        return NULL;
    }
    IrModule *mod = ir_build_module(ast, NULL, arena);
    if (mod && !opt_verify(mod))
    {
        fprintf(stderr, "  [testdriver] opt_verify failed for: %s\n", src);
        return NULL;
    }
    return mod;
}

/* tc_build_module over a concrete file path with -I-style include dirs, so
   self-compile sources (`#include "util/sbuf.h"` from src/...) resolve. */
IrModule *tc_build_module_with_dirs(const char *src, const char *file, const char *const *dirs,
                                    size_t ndirs, Arena *arena)
{
    type_reset();
    Pp *pp = pp_new(arena_new());
    for (size_t i = 0; i < ndirs; i++)
    {
        vec_push(pp->cfg.include_paths, (void *) dirs[i]);
    }
    Vec *soup = pp_preprocess(pp, file ? file : "<test>", src);
    if (!soup)
    {
        pp_free(pp);
        return NULL;
    }
    LexResult lexed = lex_finalize(soup, arena);
    pp_free(pp);
    if (!lexed.tokens)
    {
        return NULL;
    }
    ASTNode *ast = parse(lexed.tokens, lexed.count, &(ParserConfig) {0}, arena);
    if (!ast)
    {
        return NULL;
    }
    ast = semantic_check(ast, &(SemanticConfig) {0}, arena);
    if (!ast)
    {
        return NULL;
    }
    IrModule *mod = ir_build_module(ast, NULL, arena);
    if (mod && !opt_verify(mod))
    {
        fprintf(stderr, "  [testdriver] opt_verify failed for: %s\n", src);
        return NULL;
    }
    return mod;
}

i64 tc_run_interp(const char *src)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module(src, arena);
    if (!mod)
    {
        fprintf(stderr, "  [testdriver] build failed for: %s\n", src);
        test_fail();
        arena_free(arena);
        return 0;
    }
    i64 result = ir_interp_run(mod);
    arena_free(arena);
    return result;
}

static unsigned int tc_temp_seq;

static void tc_temp_path(char *buf, size_t buf_sz, const char *suffix)
{
    snprintf(buf, buf_sz, "/tmp/ficc_%06u_%s", tc_temp_seq++, suffix);
}

static void tc_temp_cleanup(char *paths[], size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        unlink(paths[i]);
    }
}

static int tc_run_elf_cfg(const char *src, CodegenBackend backend)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module(src, arena);
    if (!mod)
    {
        fprintf(stderr, "  [testdriver] build failed for: %s\n", src);
        test_fail();
        arena_free(arena);
        return -1;
    }

    CodegenConfig ccfg = {.debug = false, .backend = backend};
    CodegenModule *cm = codegen_ir_to_machine(mod, &ccfg, arena);
    if (!cm)
    {
        fprintf(stderr, "  [testdriver] codegen failed for: %s\n", src);
        test_fail();
        arena_free(arena);
        return -1;
    }

    char obj[256], bin[256];
    tc_temp_path(obj, sizeof(obj), "main.o");
    tc_temp_path(bin, sizeof(bin), "bin");
    elf_write(cm, obj, NULL);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj, bin, bin);
    int rc = tc_run_shell(cmd);

    char *paths[] = {obj, bin};
    tc_temp_cleanup(paths, 2);
    arena_free(arena);
    return rc;
}

static CodegenBackend tc_backend = CG_STACK;

void tc_set_codegen_backend(CodegenBackend backend)
{
    tc_backend = backend;
}

int tc_run_elf(const char *src)
{
    return tc_run_elf_cfg(src, tc_backend);
}

int tc_run_elf_with_extra_tu(const char *src, const char *extra_src)
{
    Arena *arena = arena_new();
    IrModule *mod = tc_build_module(src, arena);
    if (!mod)
    {
        fprintf(stderr, "  [testdriver] build failed for: %s\n", src);
        test_fail();
        arena_free(arena);
        return -1;
    }

    CodegenModule *cm = codegen_ir_to_machine(mod, NULL, arena);
    if (!cm)
    {
        fprintf(stderr, "  [testdriver] codegen failed for: %s\n", src);
        test_fail();
        arena_free(arena);
        return -1;
    }

    /* The test's own TUs live here; the ficc-compiled unit links against
       symbols they define. All files are unique per call and removed below. */
    char main_o[256], extra_c[256], extra_o[256], bin[256];
    tc_temp_path(main_o, sizeof(main_o), "main.o");
    tc_temp_path(extra_c, sizeof(extra_c), "extra.c");
    tc_temp_path(extra_o, sizeof(extra_o), "extra.o");
    tc_temp_path(bin, sizeof(bin), "bin");
    elf_write(cm, main_o, NULL);

    FILE *f = fopen(extra_c, "w");
    if (!f)
    {
        fprintf(stderr, "  [testdriver] cannot write %s\n", extra_c);
        test_fail();
        unlink(main_o);
        arena_free(arena);
        return -1;
    }
    fputs(extra_src, f);
    fclose(f);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "gcc -c %s -o %s >/dev/null 2>&1", extra_c, extra_o);
    int rc = tc_run_shell(cmd);
    if (rc == 0)
    {
        snprintf(cmd, sizeof(cmd), "gcc -no-pie %s %s -o %s >/dev/null 2>&1 && %s", main_o, extra_o,
                 bin, bin);
        rc = tc_run_shell(cmd);
    }

    char *paths[] = {main_o, extra_c, extra_o, bin};
    tc_temp_cleanup(paths, 4);
    arena_free(arena);
    return rc;
}
