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
#include "util/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_shell(const char *cmd)
{
    int rc = system(cmd);
    if (rc == -1)
    {
        return -1;
    }
    return WEXITSTATUS(rc);
}

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
    snprintf(cmd, sizeof(cmd), "gcc -no-pie %s -o %s", obj_path, bin_path);
    if (run_shell(cmd) != 0)
    {
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "%s", bin_path);
    return run_shell(cmd);
}

static RecordField *make_field(Arena *a, const char *name, Type *t)
{
    RecordField *rf = arena_alloc(a, sizeof(RecordField), _Alignof(RecordField));
    rf->name = name;
    rf->type = t;
    rf->offset = 0;
    return rf;
}

TEST(phase7, struct_size_align)
{
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P7SizeAlign");
    Vec *fields = vec_new(a);
    vec_push(fields, make_field(a, "i", type_int()));
    vec_push(fields, make_field(a, "c", type_char()));
    type_record_complete(s, fields);
    EXPECT_EQ(s->size, 8);
    EXPECT_EQ(s->align, 4);
    EXPECT_EQ(type_record_field_offset(s, "i"), 0);
    EXPECT_EQ(type_record_field_offset(s, "c"), 4);
    arena_free(a);
}

TEST(phase7, struct_padding)
{
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P7Padding");
    Vec *fields = vec_new(a);
    vec_push(fields, make_field(a, "c", type_char()));
    vec_push(fields, make_field(a, "i", type_int()));
    type_record_complete(s, fields);
    EXPECT_EQ(type_record_field_offset(s, "c"), 0);
    EXPECT_EQ(type_record_field_offset(s, "i"), 4);
    EXPECT_EQ(s->size, 8);
    EXPECT_EQ(s->align, 4);
    arena_free(a);
}

TEST(phase7, struct_self_ref)
{
    Arena *a = arena_new();
    Type *node = type_record(TYPE_STRUCT, "P7Node");
    Type *ptr_to_node = type_ptr(node);
    Vec *fields = vec_new(a);
    vec_push(fields, make_field(a, "val", type_int()));
    vec_push(fields, make_field(a, "next", ptr_to_node));
    type_record_complete(node, fields);
    EXPECT_TRUE(type_is_complete(node));
    EXPECT_EQ(node->size, 16);
    EXPECT_EQ(node->align, 8);
    EXPECT_EQ(type_record_field_offset(node, "next"), 8);
    EXPECT_TRUE(type_record_field(node, "next") == ptr_to_node);
    arena_free(a);
}

TEST(phase7, type_intern_tag)
{
    Type *a = type_record(TYPE_STRUCT, "P7InternTag");
    Type *b = type_record(TYPE_STRUCT, "P7InternTag");
    EXPECT_TRUE(a == b);
}

TEST(phase7, incomplete_until_complete)
{
    Type *s = type_record(TYPE_STRUCT, "P7Incomplete");
    EXPECT_FALSE(type_is_complete(s));
    EXPECT_EQ(s->size, 0);
}

TEST(phase7, interp_struct_member)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt p; p.x = 3; p.y = 4; return p.x * 10 + p.y; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 34);
    arena_free(arena);
}

TEST(phase7, interp_nested_member)
{
    const char *src =
        "struct In { int v; };\n"
        "struct Out { struct In in; int w; };\n"
        "int main(void) { struct Out o; o.in.v = 5; o.w = 10; return o.in.v + o.w; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 15);
    arena_free(arena);
}

TEST(phase7, interp_struct_array_member)
{
    const char *src = "struct Rec { int a[3]; };\n"
                      "int main(void) { struct Rec r; r.a[0] = 1; r.a[1] = 2; r.a[2] = 3; "
                      "return r.a[0] + r.a[1] + r.a[2]; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 6);
    arena_free(arena);
}

TEST(phase7, interp_arrow)
{
    const char *src = "struct Node { int val; struct Node *next; };\n"
                      "int main(void) { struct Node a; struct Node b; a.val = 1; b.val = 2; "
                      "a.next = &b; return a.val + a.next->val; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 3);
    arena_free(arena);
}

TEST(phase7, interp_struct_assign)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt a; struct Pt b; a.x = 1; a.y = 2; b = a; "
                      "b.x = 100; return a.x + a.y; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 3);
    arena_free(arena);
}

TEST(phase7, interp_struct_byval_arg)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int mut(struct Pt p) { p.x = p.x + 1; p.y = p.y + 1; return p.x + p.y; }\n"
                      "int main(void) { struct Pt a; a.x = 10; a.y = 20; "
                      "int r = mut(a); return a.x + a.y + r; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 62);
    arena_free(arena);
}

TEST(phase7, interp_struct_return)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "struct Pt make(int a, int b) { struct Pt p; p.x = a; p.y = b; return p; }\n"
                      "int main(void) { struct Pt p = make(7, 8); return p.x * 10 + p.y; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 78);
    arena_free(arena);
}

TEST(phase7, interp_struct_array)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt arr[3]; "
                      "arr[0].x = 1; arr[0].y = 2; arr[1].x = 3; arr[1].y = 4; "
                      "arr[2].x = 5; arr[2].y = 6; return arr[0].x + arr[1].x + arr[2].x; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 9);
    arena_free(arena);
}

TEST(phase7, interp_arrow_loop)
{
    const char *src = "struct Cell { int v; };\n"
                      "int main(void) { struct Cell cells[5]; int i; "
                      "for (i = 0; i < 5; i = i + 1) cells[i].v = i * 2; "
                      "struct Cell *p = &cells[3]; return p->v; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 6);
    arena_free(arena);
}

TEST(phase7, interp_deref_addr_record)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt q; q.x = 42; q.y = 7; return (*&q).y; }\n";
    Arena *arena = arena_new();
    EXPECT_EQ(run_interp(src, arena), 7);
    arena_free(arena);
}

TEST(phase7, elf_struct_member)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt p; p.x = 3; p.y = 4; return p.x * 10 + p.y; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_member.o", "/tmp/ficc_p7_member");
    EXPECT_EQ(rc, 34);
    unlink("/tmp/ficc_p7_member.o");
    unlink("/tmp/ficc_p7_member");
    arena_free(arena);
}

TEST(phase7, elf_struct_padding)
{
    const char *src = "struct Pair { char a; int b; };\n"
                      "int sum(struct Pair p) { return p.a + p.b; }\n"
                      "int main(void) { struct Pair p; p.a = 5; p.b = 100; return sum(p); }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_pad.o", "/tmp/ficc_p7_pad");
    EXPECT_EQ(rc, 105);
    unlink("/tmp/ficc_p7_pad.o");
    unlink("/tmp/ficc_p7_pad");
    arena_free(arena);
}

TEST(phase7, elf_struct_pointer)
{
    const char *src = "struct Node { int val; struct Node *next; };\n"
                      "int main(void) { struct Node a; struct Node b; a.val = 1; b.val = 2; "
                      "a.next = &b; return a.val + a.next->val; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_ptr.o", "/tmp/ficc_p7_ptr");
    EXPECT_EQ(rc, 3);
    unlink("/tmp/ficc_p7_ptr.o");
    unlink("/tmp/ficc_p7_ptr");
    arena_free(arena);
}

TEST(phase7, elf_struct_byval)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "struct Pt bump(struct Pt p) { p.x = p.x + 1; p.y = p.y + 1; return p; }\n"
                      "int main(void) { struct Pt a; a.x = 10; a.y = 20; "
                      "struct Pt b = bump(a); return a.x + a.y + b.x + b.y; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_byval.o", "/tmp/ficc_p7_byval");
    EXPECT_EQ(rc, 62);
    unlink("/tmp/ficc_p7_byval.o");
    unlink("/tmp/ficc_p7_byval");
    arena_free(arena);
}

TEST(phase7, elf_struct_return)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "struct Pt make(int a, int b) { struct Pt p; p.x = a; p.y = b; return p; }\n"
                      "int main(void) { struct Pt p = make(7, 8); return p.x * 10 + p.y; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_ret.o", "/tmp/ficc_p7_ret");
    EXPECT_EQ(rc, 78);
    unlink("/tmp/ficc_p7_ret.o");
    unlink("/tmp/ficc_p7_ret");
    arena_free(arena);
}

TEST(phase7, elf_struct_array)
{
    const char *src = "struct Pt { int x; int y; };\n"
                      "int main(void) { struct Pt arr[3]; "
                      "arr[0].x = 1; arr[0].y = 2; arr[1].x = 3; arr[1].y = 4; "
                      "arr[2].x = 5; arr[2].y = 6; return arr[0].x + arr[1].x + arr[2].x; }\n";
    Arena *arena = arena_new();
    int rc = run_elf(src, arena, "/tmp/ficc_p7_arr.o", "/tmp/ficc_p7_arr");
    EXPECT_EQ(rc, 9);
    unlink("/tmp/ficc_p7_arr.o");
    unlink("/tmp/ficc_p7_arr");
    arena_free(arena);
}

TEST(phase7, negative_unknown_member)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt p; return p.y; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_dot_on_non_record)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("int main(void) { int x; return x.y; }\n", arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_arrow_on_non_pointer)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt p; return p->x; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_sizeof_incomplete)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt;\n"
                                  "int main(void) { return sizeof(struct Pt); }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_redefined_tag)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "struct Pt { int y; };\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_arith_on_record)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt a; struct Pt b; return a + b; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_unary_on_record)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt p; return -p; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_ternary_on_record)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt a; struct Pt b; "
                                  "return (1 ? a : b).x; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_record_init_scalar)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "int main(void) { struct Pt p = 5; return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}

TEST(phase7, negative_record_return_scalar)
{
    Arena *arena = arena_new();
    EXPECT_TRUE(build_from_source("struct Pt { int x; };\n"
                                  "struct Pt f(void) { return 5; }\n"
                                  "int main(void) { return 0; }\n",
                                  arena) == NULL);
    arena_free(arena);
}
