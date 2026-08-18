#include "codegen.h"
#include "elf.h"
#include "harness.h"
#include "ir_builder.h"
#include "ir_interp.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "type.h"
#include "util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_shell_cmd(const char *cmd);

static IrModule *build_from_source(const char *src, Arena *arena)
{
    type_reset();
    LexResult lexed = lex("<test>", src, arena);
    if (!lexed.tokens)
    {
        return NULL;
    }
    ASTNode *ast = parse(lexed.tokens, lexed.count, arena);
    if (!ast)
    {
        return NULL;
    }
    ast = semantic_check(ast, arena);
    if (!ast)
    {
        return NULL;
    }
    return ir_build_module(ast, arena);
}

static i64 run_interp(const char *src, Arena *arena)
{
    IrModule *mod = build_from_source(src, arena);
    if (!mod)
    {
        return 0;
    }
    return ir_interp_run(mod);
}

static int run_elf(const char *src, Arena *arena, const char *obj_path, const char *bin_path)
{
    IrModule *mod = build_from_source(src, arena);
    if (!mod)
    {
        return -1;
    }
    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    if (!cm)
    {
        return -1;
    }
    elf_write(cm, obj_path);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s >/dev/null 2>&1 && %s", obj_path, bin_path,
             bin_path);
    return run_shell_cmd(cmd);
}

static int run_shell_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
    {
        return -1;
    }
    return WEXITSTATUS(rc);
}

/* --- positive: reading const lvalues --- */

TEST(phase9, const_local_read)
{
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp("int main(void) { const int x = 5; return x + 1; }\n", arena), 6);
    arena_free(arena);
}

TEST(phase9, const_local_spelling_int_const)
{
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp("int main(void) { int const x = 5; return x + 1; }\n", arena), 6);
    arena_free(arena);
}

TEST(phase9, const_block_static_read)
{
    Arena *arena = arena_new();
    const char *src = "int f(int x) { static const int k = 7; return x + k; }\n"
                      "int main(void) { return f(5); }\n";
    EXPECT_EQ(run_interp(src, arena), 12);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_bs.o", "/tmp/ficc_p9_bs"), 12);
    arena_free(arena);
    unlink("/tmp/ficc_p9_bs.o");
    unlink("/tmp/ficc_p9_bs");
}

TEST(phase9, const_global_read)
{
    Arena *arena = arena_new();
    const char *src = "const int g = 42;\nint main(void) { return g; }\n";
    EXPECT_EQ(run_interp(src, arena), 42);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cg.o", "/tmp/ficc_p9_cg"), 42);
    arena_free(arena);
    unlink("/tmp/ficc_p9_cg.o");
    unlink("/tmp/ficc_p9_cg");
}

TEST(phase9, const_global_zero_init)
{
    Arena *arena = arena_new();
    const char *src = "const int z;\nint main(void) { return z + 3; }\n";
    EXPECT_EQ(run_interp(src, arena), 3);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cgz.o", "/tmp/ficc_p9_cgz"), 3);
    arena_free(arena);
    unlink("/tmp/ficc_p9_cgz.o");
    unlink("/tmp/ficc_p9_cgz");
}

TEST(phase9, const_global_string_pointer)
{
    Arena *arena = arena_new();
    const char *src = "const char * const p = \"hi\";\n"
                      "int main(void) { return p[1]; }\n";
    EXPECT_EQ(run_interp(src, arena), 'i');
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cp.o", "/tmp/ficc_p9_cp"), 'i');
    arena_free(arena);
    unlink("/tmp/ficc_p9_cp.o");
    unlink("/tmp/ficc_p9_cp");
}

TEST(phase9, const_array_elements)
{
    Arena *arena = arena_new();
    const char *src = "const int a[3];\n"
                      "int sum(const int arr[3]) { return arr[0] + arr[2]; }\n"
                      "int main(void) { return sum(a); }\n";
    EXPECT_EQ(run_interp(src, arena), 0);
    arena_free(arena);
}

TEST(phase9, struct_const_pointer_arg)
{
    Arena *arena = arena_new();
    const char *src = "struct point { int x; int y; };\n"
                      "int sum(struct point const *p) { return p->x + p->y; }\n"
                      "int main(void) { struct point pt; pt.x = 3; pt.y = 4; return sum(&pt); }\n";
    EXPECT_EQ(run_interp(src, arena), 7);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cs.o", "/tmp/ficc_p9_cs"), 7);
    arena_free(arena);
    unlink("/tmp/ficc_p9_cs.o");
    unlink("/tmp/ficc_p9_cs");
}

TEST(phase9, const_struct_value_assignment_is_read_only)
{
    Arena *arena = arena_new();
    const char *src = "struct point { int x; int y; };\n"
                      "int main(void) { struct point a; a.x = 5; a.y = 7;\n"
                      "  const struct point b = a; struct point c; c = b; "
                      "return c.y; }\n";
    EXPECT_EQ(run_interp(src, arena), 7);
    arena_free(arena);
}

TEST(phase9, const_pointer_to_scalar_global)
{
    Arena *arena = arena_new();
    const char *src = "int g = 100;\n"
                      "int main(void) { const int *p = &g; return *p - 50; }\n";
    EXPECT_EQ(run_interp(src, arena), 50);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cps.o", "/tmp/ficc_p9_cps"), 50);
    arena_free(arena);
    unlink("/tmp/ficc_p9_cps.o");
    unlink("/tmp/ficc_p9_cps");
}

TEST(phase9, add_qualifier_is_allowed)
{
    Arena *arena = arena_new();
    const char *src = "int g = 7;\n"
                      "void take(const int *p) { g = g + *p; }\n"
                      "int main(void) { int *q = &g; take(q); return g; }\n";
    EXPECT_EQ(run_interp(src, arena), 14);
    arena_free(arena);
}

TEST(phase9, const_pointer_write_through_ok)
{
    Arena *arena = arena_new();
    const char *src = "int g = 5;\n"
                      "int main(void) { int * const cp = &g; *cp = 9; return g; }\n";
    EXPECT_EQ(run_interp(src, arena), 9);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_cw.o", "/tmp/ficc_p9_cw"), 9);
    arena_free(arena);
    unlink("/tmp/ficc_p9_cw.o");
    unlink("/tmp/ficc_p9_cw");
}

TEST(phase9, const_char_ptr_from_string_literal)
{
    Arena *arena = arena_new();
    const char *src = "const char *s = \"hey\";\n"
                      "int main(void) { return s[2]; }\n";
    EXPECT_EQ(run_interp(src, arena), 'y');
    arena_free(arena);
}

TEST(phase9, const_param_qualifiers_ignored_not_prototypes)
{
    /* Top-level param const is enforced in the body but does not change the
       signature; no prototypes exist yet so a single definition is tested. */
    Arena *arena = arena_new();
    const char *src = "int f(const int x) { return x * 2; }\n"
                      "int main(void) { return f(21); }\n";
    EXPECT_EQ(run_interp(src, arena), 42);
    arena_free(arena);
}

/* --- positive: .rodata placement (no SHF_WRITE) --- */

TEST(phase9, const_global_in_rodata)
{
    Arena *arena = arena_new();
    IrModule *mod = build_from_source("const int g = 42;\nint main(void) { return g; }\n", arena);
    EXPECT_TRUE(mod != NULL);
    EXPECT_EQ(vec_size(mod->globals), 1);
    IrGlobal *g = (IrGlobal *) vec_get(mod->globals, 0);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);
    arena_free(arena);
}

TEST(phase9, const_zero_global_in_rodata_not_bss)
{
    Arena *arena = arena_new();
    IrModule *mod = build_from_source("const int z;\nint main(void) { return z; }\n", arena);
    EXPECT_TRUE(mod != NULL);
    IrGlobal *g = (IrGlobal *) vec_get(mod->globals, 0);
    EXPECT_EQ(g->section, IR_SECTION_RODATA);
    EXPECT_TRUE(g->init_data != NULL);
    EXPECT_EQ(g->init_len, (size_t) 4);
    arena_free(arena);
}

TEST(phase9, const_global_elf_rodata_no_write_flag)
{
    Arena *arena = arena_new();
    IrModule *mod = build_from_source("const int g = 42;\nint main(void) { return g; }\n", arena);
    EXPECT_TRUE(mod != NULL);
    if (!mod)
    {
        arena_free(arena);
        return;
    }
    CodegenModule *cm = codegen_ir_to_machine(mod, arena);
    EXPECT_TRUE(cm != NULL);
    if (!cm)
    {
        arena_free(arena);
        return;
    }
    elf_write(cm, "/tmp/ficc_p9_rd.o");
    int rc = run_shell_cmd("objdump -h /tmp/ficc_p9_rd.o | grep -q '.rodata' && "
                           "! objdump -h /tmp/ficc_p9_rd.o | grep '.rodata' | grep -q WRITE");
    EXPECT_EQ(rc, 0);
    arena_free(arena);
    unlink("/tmp/ficc_p9_rd.o");
}

/* --- negative: writes through const lvalues --- */

TEST(phase9, negative_assign_const_var)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(
        build_from_source("int main(void) { const int x = 5; x = 6; return x; }\n", arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_assign_via_const_ptr)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g = 5;\n"
                                  "int main(void) { const int *p = &g; *p = 6; return g; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_write_const_array_elem)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("const int a[3];\n"
                                  "int main(void) { a[1] = 2; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_write_const_struct_member)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct point { int x; int y; };\n"
                                  "int set(struct point const *p) { p->x = 9; return 0; }\n"
                                  "int main(void) { struct point pt; return set(&pt); }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_write_const_member_dot)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct point { int x; int y; };\n"
                                  "int main(void) { const struct point pt; pt.x = 1; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_write_const_field_of_const_struct)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct box { const int w; };\n"
                                  "int main(void) { struct box b; b.w = 3; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_rebind_const_pointer)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g;\n"
                                  "int main(void) { int * const cp = &g; cp = &g; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_write_const_local_param)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("void f(const int x) { x = 1; }\n"
                                  "int main(void) { f(0); return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

/* --- negative: discarding qualifiers (C11 §6.5.16.1) --- */

TEST(phase9, negative_discard_in_initializer)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g = 5;\n"
                                  "int main(void) { const int *p = &g; int *q = p; return *q; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_discard_in_assignment)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g = 5;\n"
                                  "int main(void) { const int *p = &g; int *q; q = p; "
                                  "return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_discard_in_argument)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g = 5;\n"
                                  "void f(int *p) { (void) p; }\n"
                                  "int main(void) { const int *q = &g; f(q); return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_discard_in_return)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g = 5;\n"
                                  "int *f(void) { const int *p = &g; return p; }\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_multi_level_discard)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g; int *gp;\n"
                                  "int main(void) { int **pp = &gp; const int **cpp = pp; "
                                  "return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_int_ptr_to_char_ptr)
{
    /* Different pointee types are still incompatible even unqualified. */
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int g; char *cp;\n"
                                  "int main(void) { cp = &g; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_conflicting_qualifiers_file_scope)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int x;\nconst int x;\nint main(void) { return 0; }\n", arena) ==
                NULL);
    arena_free(arena);
}

TEST(phase9, negative_consistency_ok_same_qualifiers)
{
    /* Same qualifiers merge fine (tentative definitions). */
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("const int x = 5;\nconst int x;\nint main(void) { return x; }\n",
                                  arena) != NULL);
    arena_free(arena);
}

/* --- Part 9b: scalar address-of (`&x`) --- */

TEST(phase9, addr_of_scalar_write_through)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int x = 3; int *p = &x; *p = 7; return x; }\n";
    EXPECT_EQ(run_interp(src, arena), 7);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao1.o", "/tmp/ficc_p9_ao1"), 7);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao1.o");
    unlink("/tmp/ficc_p9_ao1");
}

TEST(phase9, addr_of_const_scalar)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { const int x = 41; const int *p = &x; return *p + 1; }\n";
    EXPECT_EQ(run_interp(src, arena), 42);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao2.o", "/tmp/ficc_p9_ao2"), 42);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao2.o");
    unlink("/tmp/ficc_p9_ao2");
}

TEST(phase9, addr_of_in_loop)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int i = 0; int s = 0; int *p = &i;\n"
                      "  while (i < 4) { i = i + 1; s = s + i; } return s + i; }\n";
    EXPECT_EQ(run_interp(src, arena), 14);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao3.o", "/tmp/ficc_p9_ao3"), 14);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao3.o");
    unlink("/tmp/ficc_p9_ao3");
}

TEST(phase9, addr_of_param)
{
    Arena *arena = arena_new();
    const char *src = "void bump(int *p) { *p = *p + 1; }\n"
                      "int main(void) { int v = 9; bump(&v); return v; }\n";
    EXPECT_EQ(run_interp(src, arena), 10);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao4.o", "/tmp/ficc_p9_ao4"), 10);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao4.o");
    unlink("/tmp/ficc_p9_ao4");
}

TEST(phase9, addr_of_pointer_to_pointer)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int x = 2; int *p = &x; int **q = &p; **q = 6; "
                      "return x; }\n";
    EXPECT_EQ(run_interp(src, arena), 6);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao5.o", "/tmp/ficc_p9_ao5"), 6);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao5.o");
    unlink("/tmp/ficc_p9_ao5");
}

TEST(phase9, addr_of_nested_block)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int r = 0; { int y = 3; int *p = &y; *p = 9; "
                      "r = y; } return r; }\n";
    EXPECT_EQ(run_interp(src, arena), 9);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao6.o", "/tmp/ficc_p9_ao6"), 9);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao6.o");
    unlink("/tmp/ficc_p9_ao6");
}

TEST(phase9, addr_of_deref_of_addr)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int x = 1; *&x = 5; return x; }\n";
    EXPECT_EQ(run_interp(src, arena), 5);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao7.o", "/tmp/ficc_p9_ao7"), 5);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao7.o");
    unlink("/tmp/ficc_p9_ao7");
}

TEST(phase9, addr_of_struct_const_ptr_from_local)
{
    Arena *arena = arena_new();
    const char *src = "struct point { int x; int y; };\n"
                      "int main(void) { struct point pt; pt.x = 20; pt.y = 22;\n"
                      "  const struct point *p = &pt; return p->x + p->y; }\n";
    EXPECT_EQ(run_interp(src, arena), 42);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao8.o", "/tmp/ficc_p9_ao8"), 42);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao8.o");
    unlink("/tmp/ficc_p9_ao8");
}

TEST(phase9, addr_of_ifelse_through_pointer)
{
    Arena *arena = arena_new();
    const char *src = "int main(void) { int v = 0; int *p = &v;\n"
                      "  if (1) { *p = 11; } else { *p = 22; } return v; }\n";
    EXPECT_EQ(run_interp(src, arena), 11);
    EXPECT_EQ(run_elf(src, arena, "/tmp/ficc_p9_ao9.o", "/tmp/ficc_p9_ao9"), 11);
    arena_free(arena);
    unlink("/tmp/ficc_p9_ao9.o");
    unlink("/tmp/ficc_p9_ao9");
}

TEST(phase9, negative_addr_of_const_discard)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { const int x = 5; int *p = &x; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, negative_addr_of_const_write_through)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { const int x = 5; const int *p = &x; "
                                  "*p = 6; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase9, addr_of_spills_to_alloca)
{
    /* The spill slot alloca must land in the function's entry block. */
    Arena *arena = arena_new();
    IrModule *mod =
        build_from_source("int main(void) { int x = 3; int *p = &x; return *p; }\n", arena);
    EXPECT_TRUE(mod != NULL);
    IrFunction *f = (IrFunction *) vec_get(mod->funcs, 0);
    IrBlock *entry = (IrBlock *) vec_get(f->blocks, 0);
    bool found_alloca = false;
    size_t n = vec_size(entry->instrs);
    for (size_t i = 0; i < n; i++)
    {
        IrInstr *in = (IrInstr *) vec_get(entry->instrs, i);
        if (in->opcode == OP_ALLOCA)
        {
            found_alloca = true;
        }
    }
    EXPECT_TRUE(found_alloca);
    arena_free(arena);
}