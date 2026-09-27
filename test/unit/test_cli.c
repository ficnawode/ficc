#include "cli.h"
#include "harness.h"
#include "util/arena.h"
#include "util/vec.h"

#include <string.h>

#define ARRAY_LEN(a) ((int) (sizeof(a) / sizeof((a)[0])))

static const char *input_arg(CompilerConfig *cfg, size_t i)
{
    return (const char *) vec_get(cfg->inputs, i);
}

static PPCommand *cmd_at(CompilerConfig *cfg, size_t i)
{
    return (PPCommand *) vec_get(cfg->pp.cmds, i);
}

static const char *include_path_at(CompilerConfig *cfg, size_t i)
{
    return (const char *) vec_get(cfg->pp.include_paths, i);
}

TEST(cli, single_input)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->inputs), 1);
    EXPECT_STR_EQ(input_arg(cfg, 0), "a.c");
    EXPECT_STR_EQ(cfg->exe_path, "ficc");
    EXPECT_NULL(cfg->output_path);
    EXPECT_FALSE(cfg->emit_pp);
    EXPECT_FALSE(cfg->emit_obj);
    EXPECT_FALSE(cfg->dump_ast);
    arena_free(a);
}

TEST(cli, e_flag_emits_pp)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-E", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->emit_pp);
    arena_free(a);
}

TEST(cli, c_flag_emits_obj)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-c", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->emit_obj);
    arena_free(a);
}

TEST(cli, run_flag_runs_interp)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-run", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->run_interp);
    arena_free(a);
}

TEST(cli, tokens_flag_dumps_tokens)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-tokens", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->dump_tokens);
    arena_free(a);
}

TEST(cli, pp_flag_dumps_pp)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-pp", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->dump_pp);
    arena_free(a);
}

TEST(cli, ast_flag_dumps_ast)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-ast", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->dump_ast);
    arena_free(a);
}

TEST(cli, ir_flag_dumps_ir)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-ir", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->dump_ir);
    arena_free(a);
}

TEST(cli, nostdinc_flag_sets_pp_nostdinc)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-nostdinc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->pp.nostdinc);
    arena_free(a);
}

TEST(cli, pp_flags_default_off)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_FALSE(cfg->pp.keep_comments);
    EXPECT_FALSE(cfg->pp.no_markers);
    arena_free(a);
}

TEST(cli, cap_keeps_comments_and_implies_pp)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-C", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->emit_pp);
    EXPECT_TRUE(cfg->pp.keep_comments);
    EXPECT_FALSE(cfg->pp.no_markers);
    arena_free(a);
}

TEST(cli, p_omits_markers_and_implies_pp)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-P", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->emit_pp);
    EXPECT_TRUE(cfg->pp.no_markers);
    EXPECT_FALSE(cfg->pp.keep_comments);
    arena_free(a);
}

TEST(cli, pedantic_tandem)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-fpedantic", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->pp.pedantic);
    EXPECT_TRUE(cfg->parser.pedantic);
    EXPECT_TRUE(cfg->semantic.pedantic);
    arena_free(a);
}

TEST(cli, pedantic_last_wins)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-fpedantic", "-fno-pedantic", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_FALSE(cfg->pp.pedantic);
    EXPECT_FALSE(cfg->parser.pedantic);
    EXPECT_FALSE(cfg->semantic.pedantic);
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-fno-pedantic", "-fpedantic", "-fno-pedantic", "a.c"};
    CompilerConfig *cfg2 = cli_parse(ARRAY_LEN(argv2), argv2, a2);
    EXPECT_NOTNULL(cfg2);
    EXPECT_FALSE(cfg2->semantic.pedantic);
    arena_free(a2);
}

TEST(cli, unknown_f_flags)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-ffoo", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-fno-unknown", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv2), argv2, a2));
    arena_free(a2);
}

TEST(cli, include_path_forms)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-I", "dir1", "-Idir2", "-I=dir3", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.include_paths), 3);
    EXPECT_STR_EQ(include_path_at(cfg, 0), "dir1");
    EXPECT_STR_EQ(include_path_at(cfg, 1), "dir2");
    EXPECT_STR_EQ(include_path_at(cfg, 2), "dir3");
    arena_free(a);
}

static const char *system_include_path_at(CompilerConfig *cfg, size_t i)
{
    return (const char *) vec_get(cfg->pp.system_include_paths, i);
}

TEST(cli, system_include_path_forms)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-isystem", "sys1", "-isystem=sys2", "-isystemsys3", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.system_include_paths), 3);
    EXPECT_STR_EQ(system_include_path_at(cfg, 0), "sys1");
    EXPECT_STR_EQ(system_include_path_at(cfg, 1), "sys2");
    EXPECT_STR_EQ(system_include_path_at(cfg, 2), "sys3");
    EXPECT_EQ(vec_size(cfg->pp.include_paths), 0);
    arena_free(a);
}

TEST(cli, isystem_kept_separate_from_I)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-I", "user", "-isystem", "sys", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.include_paths), 1);
    EXPECT_STR_EQ(include_path_at(cfg, 0), "user");
    EXPECT_EQ(vec_size(cfg->pp.system_include_paths), 1);
    EXPECT_STR_EQ(system_include_path_at(cfg, 0), "sys");
    arena_free(a);
}

TEST(cli, define_forms_and_order)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-D", "A", "-DB", "-D=C", "-DZ=9", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.cmds), 4);
    EXPECT_EQ(cmd_at(cfg, 0)->kind, CMD_DEFINE);
    EXPECT_STR_EQ(cmd_at(cfg, 0)->arg, "A");
    EXPECT_EQ(cmd_at(cfg, 1)->kind, CMD_DEFINE);
    EXPECT_STR_EQ(cmd_at(cfg, 1)->arg, "B");
    EXPECT_EQ(cmd_at(cfg, 2)->kind, CMD_DEFINE);
    EXPECT_STR_EQ(cmd_at(cfg, 2)->arg, "C");
    EXPECT_EQ(cmd_at(cfg, 3)->kind, CMD_DEFINE);
    EXPECT_STR_EQ(cmd_at(cfg, 3)->arg, "Z=9");
    arena_free(a);
}

TEST(cli, undef_forms)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-U", "A", "-UB", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.cmds), 2);
    EXPECT_EQ(cmd_at(cfg, 0)->kind, CMD_UNDEF);
    EXPECT_STR_EQ(cmd_at(cfg, 0)->arg, "A");
    EXPECT_EQ(cmd_at(cfg, 1)->kind, CMD_UNDEF);
    EXPECT_STR_EQ(cmd_at(cfg, 1)->arg, "B");
    arena_free(a);
}

TEST(cli, include_file_forms)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-include", "h1.h", "-include=h2.h", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.cmds), 2);
    EXPECT_EQ(cmd_at(cfg, 0)->kind, CMD_INCLUDE);
    EXPECT_STR_EQ(cmd_at(cfg, 0)->arg, "h1.h");
    EXPECT_EQ(cmd_at(cfg, 1)->kind, CMD_INCLUDE);
    EXPECT_STR_EQ(cmd_at(cfg, 1)->arg, "h2.h");
    arena_free(a);
}

TEST(cli, define_undef_order)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-D", "A", "-U", "A", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.cmds), 2);
    EXPECT_EQ(cmd_at(cfg, 0)->kind, CMD_DEFINE);
    EXPECT_EQ(cmd_at(cfg, 1)->kind, CMD_UNDEF);
    arena_free(a);
}

TEST(cli, output_forms)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-o", "out.o", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_STR_EQ(cfg->output_path, "out.o");
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-oout2.o", "a.c"};
    CompilerConfig *cfg2 = cli_parse(ARRAY_LEN(argv2), argv2, a2);
    EXPECT_NOTNULL(cfg2);
    EXPECT_STR_EQ(cfg2->output_path, "out2.o");
    arena_free(a2);

    Arena *a3 = arena_new();
    char *argv3[] = {"ficc", "-o=out3.o", "a.c"};
    CompilerConfig *cfg3 = cli_parse(ARRAY_LEN(argv3), argv3, a3);
    EXPECT_NOTNULL(cfg3);
    EXPECT_STR_EQ(cfg3->output_path, "out3.o");
    arena_free(a3);
}

TEST(cli, output_last_wins)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-o", "x.o", "-oy.o", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_STR_EQ(cfg->output_path, "y.o");
    arena_free(a);
}

TEST(cli, output_multi_input_allowed)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-o", "out", "a.c", "b.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    if (cfg)
    {
        EXPECT_STR_EQ(cfg->output_path, "out");
        EXPECT_EQ(vec_size(cfg->inputs), 2);
    }
    arena_free(a);
}

TEST(cli, output_multi_input_rejected_with_c)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-c", "-o", "out.o", "a.c", "b.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);
}

TEST(cli, lib_path_flag_adds_path)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-L", "libdir", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->link.lib_paths), 1);
    EXPECT_STR_EQ((const char *) vec_get(cfg->link.lib_paths, 0), "libdir");
    arena_free(a);
}

TEST(cli, lib_flag_adds_library)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-lm", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->link.libs), 1);
    EXPECT_STR_EQ((const char *) vec_get(cfg->link.libs, 0), "m");
    arena_free(a);
}

TEST(cli, lib_flag_separate_value_adds_library)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-l", "m", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->link.libs), 1);
    EXPECT_STR_EQ((const char *) vec_get(cfg->link.libs, 0), "m");
    arena_free(a);
}

TEST(cli, nostdlib_flag_sets_link_nostdlib)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-nostdlib", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->link.nostdlib);
    arena_free(a);
}

TEST(cli, g_flag_sets_codegen_debug)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-g", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->codegen.debug);
    arena_free(a);
}

TEST(cli, static_flag_sets_link_static)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-static", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->link.static_);
    arena_free(a);
}

TEST(cli, rdynamic_flag_sets_export_dynamic)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-rdynamic", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->link.export_dynamic);
    arena_free(a);
}

TEST(cli, export_dynamic_long_alias_sets_export_dynamic)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "--export-dynamic", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->link.export_dynamic);
    arena_free(a);
}

TEST(cli, multi_input)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "a.c", "b.c", "c.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->inputs), 3);
    EXPECT_STR_EQ(input_arg(cfg, 0), "a.c");
    EXPECT_STR_EQ(input_arg(cfg, 2), "c.c");
    arena_free(a);
}

TEST(cli, dash_is_stdin)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->inputs), 1);
    EXPECT_STR_EQ(input_arg(cfg, 0), "-");
    arena_free(a);
}

TEST(cli, double_dash_terminator)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-E", "--", "-E"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->emit_pp);
    EXPECT_EQ(vec_size(cfg->inputs), 1);
    EXPECT_STR_EQ(input_arg(cfg, 0), "-E");
    arena_free(a);
}

TEST(cli, double_dash_keeps_order)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-D", "A", "--", "-E", "b.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.cmds), 1);
    EXPECT_EQ(cmd_at(cfg, 0)->kind, CMD_DEFINE);
    EXPECT_EQ(vec_size(cfg->inputs), 2);
    EXPECT_STR_EQ(input_arg(cfg, 0), "-E");
    EXPECT_STR_EQ(input_arg(cfg, 1), "b.c");
    arena_free(a);
}

TEST(cli, unknown_option)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-zzz", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);
}

TEST(cli, missing_argument)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-I"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-D"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv2), argv2, a2));
    arena_free(a2);

    Arena *a3 = arena_new();
    char *argv3[] = {"ficc", "-include"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv3), argv3, a3));
    arena_free(a3);
}

TEST(cli, include_joint_value_accepted)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-I=dir", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(vec_size(cfg->pp.include_paths), 1);
    EXPECT_STR_EQ(include_path_at(cfg, 0), "dir");
    arena_free(a);
}

TEST(cli, define_empty_joint_value_rejected)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-D=", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);
}

TEST(cli, no_inputs)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-E"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);
}

TEST(cli, exe_path)
{
    Arena *a = arena_new();
    char *argv[] = {"./build/bin/ficc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_STR_EQ(cfg->exe_path, "./build/bin/ficc");
    arena_free(a);
}

TEST(cli, help_flag_recognized)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "--help"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->show_help);
    EXPECT_EQ(vec_size(cfg->inputs), 0);
    arena_free(a);
}

TEST(cli, help_short_circuits)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-E", "--help", "foo.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_TRUE(cfg->show_help);
    EXPECT_TRUE(cfg->emit_pp);
    EXPECT_EQ(vec_size(cfg->inputs), 0);
    arena_free(a);
}

TEST(cli, help_not_set_by_default)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_FALSE(cfg->show_help);
    arena_free(a);
}

TEST(cli, help_listing_smoke)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "--help"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    cli_help(cfg);
    arena_free(a);
}

TEST(cli, opt_level_defaults_to_zero)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(cfg->opt, OPT_LEVEL_0);
    arena_free(a);
}

TEST(cli, opt_levels_explicit)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-O0", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(cfg->opt, OPT_LEVEL_0);
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-O1", "a.c"};
    CompilerConfig *cfg2 = cli_parse(ARRAY_LEN(argv2), argv2, a2);
    EXPECT_NOTNULL(cfg2);
    EXPECT_EQ(cfg2->opt, OPT_LEVEL_1);
    arena_free(a2);

    Arena *a3 = arena_new();
    char *argv3[] = {"ficc", "-O2", "a.c"};
    CompilerConfig *cfg3 = cli_parse(ARRAY_LEN(argv3), argv3, a3);
    EXPECT_NOTNULL(cfg3);
    EXPECT_EQ(cfg3->opt, OPT_LEVEL_2);
    arena_free(a3);

    Arena *a4 = arena_new();
    char *argv4[] = {"ficc", "-O3", "a.c"};
    CompilerConfig *cfg4 = cli_parse(ARRAY_LEN(argv4), argv4, a4);
    EXPECT_NOTNULL(cfg4);
    EXPECT_EQ(cfg4->opt, OPT_LEVEL_3);
    arena_free(a4);
}

TEST(cli, opt_bare_is_level_two)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-O", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(cfg->opt, OPT_LEVEL_2);
    arena_free(a);
}

TEST(cli, opt_level_last_wins)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-O2", "-O0", "a.c"};
    CompilerConfig *cfg = cli_parse(ARRAY_LEN(argv), argv, a);
    EXPECT_NOTNULL(cfg);
    EXPECT_EQ(cfg->opt, OPT_LEVEL_0);
    arena_free(a);
}

TEST(cli, unknown_opt_level_rejected)
{
    Arena *a = arena_new();
    char *argv[] = {"ficc", "-O4", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv), argv, a));
    arena_free(a);

    Arena *a2 = arena_new();
    char *argv2[] = {"ficc", "-Ofast", "a.c"};
    EXPECT_NULL(cli_parse(ARRAY_LEN(argv2), argv2, a2));
    arena_free(a2);
}
