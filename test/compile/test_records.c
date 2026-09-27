#include "ast.h"
#include "harness.h"
#include "testdriver.h"
#include "type.h"
#include "util/arena.h"
#include "util/vec.h"

#include <string.h>

static RecordField *make_field(Arena *a, const char *name, Type *t)
{
    RecordField *rf = arena_alloc(a, sizeof(RecordField), _Alignof(RecordField));
    rf->name = name;
    rf->type = t;
    rf->offset = 0;
    rf->bit_offset = -1;
    rf->bit_width = -1;
    rf->align_override = 0;
    return rf;
}

TEST(records, struct_size_align)
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

TEST(records, struct_padding)
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

TEST(records, struct_self_ref)
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

TEST(records, type_intern_tag)
{
    Type *a = type_record(TYPE_STRUCT, "P7InternTag");
    Type *b = type_record(TYPE_STRUCT, "P7InternTag");
    EXPECT_TRUE(a == b);
}

TEST(records, incomplete_until_complete)
{
    Type *s = type_record(TYPE_STRUCT, "P7Incomplete");
    EXPECT_FALSE(type_is_complete(s));
    EXPECT_EQ(s->size, 0);
}

TEST(records, union_layout)
{
    Arena *a = arena_new();
    Type *u = type_record(TYPE_UNION, "P7UnionLayout");
    Vec *fields = vec_new(a);
    vec_push(fields, make_field(a, "i", type_int()));
    vec_push(fields, make_field(a, "c", type_char()));
    vec_push(fields, make_field(a, "l", type_long()));
    type_record_complete(u, fields);
    EXPECT_TRUE(type_is_union(u));
    EXPECT_TRUE(type_is_record(u));
    EXPECT_EQ(type_record_field_offset(u, "i"), 0);
    EXPECT_EQ(type_record_field_offset(u, "c"), 0);
    EXPECT_EQ(type_record_field_offset(u, "l"), 0);
    EXPECT_EQ(u->size, 8);
    EXPECT_EQ(u->align, 8);
    arena_free(a);
}

TEST(records, union_size_align)
{
    Arena *a = arena_new();
    Type *u = type_record(TYPE_UNION, "P7UnionSizeAlign");
    Vec *fields = vec_new(a);
    vec_push(fields, make_field(a, "c", type_char()));
    vec_push(fields, make_field(a, "i", type_int()));
    type_record_complete(u, fields);
    EXPECT_EQ(u->size, 4);
    EXPECT_EQ(u->align, 4);
    arena_free(a);
}

TEST(records, union_nested_struct_layout)
{
    Arena *a = arena_new();
    Type *pt = type_record(TYPE_STRUCT, "P7UPoint");
    Vec *pt_fields = vec_new(a);
    vec_push(pt_fields, make_field(a, "x", type_int()));
    vec_push(pt_fields, make_field(a, "y", type_int()));
    type_record_complete(pt, pt_fields);

    Type *u = type_record(TYPE_UNION, "P7UNested");
    Vec *u_fields = vec_new(a);
    vec_push(u_fields, make_field(a, "p", pt));
    vec_push(u_fields, make_field(a, "l", type_long()));
    type_record_complete(u, u_fields);
    EXPECT_EQ(u->size, 8);
    EXPECT_EQ(u->align, 8);
    EXPECT_EQ(type_record_field_offset(u, "p"), 0);
    arena_free(a);
}

TEST(records, struct_nested_union_layout)
{
    Arena *a = arena_new();
    Type *u = type_record(TYPE_UNION, "P7UWrapInner");
    Vec *u_fields = vec_new(a);
    vec_push(u_fields, make_field(a, "l", type_long()));
    vec_push(u_fields, make_field(a, "c", type_char()));
    type_record_complete(u, u_fields);

    Type *s = type_record(TYPE_STRUCT, "P7UWrap");
    Vec *s_fields = vec_new(a);
    vec_push(s_fields, make_field(a, "tag", type_char()));
    vec_push(s_fields, make_field(a, "u", u));
    type_record_complete(s, s_fields);
    EXPECT_EQ(type_record_field_offset(s, "tag"), 0);
    EXPECT_EQ(type_record_field_offset(s, "u"), 8);
    EXPECT_EQ(s->size, 16);
    EXPECT_EQ(s->align, 8);
    arena_free(a);
}

TEST(records, union_intern_tag)
{
    Type *a = type_record(TYPE_UNION, "P7UInternTag");
    Type *b = type_record(TYPE_UNION, "P7UInternTag");
    EXPECT_TRUE(a == b);
}

TEST(records, union_incomplete)
{
    Type *u = type_record(TYPE_UNION, "P7UIncomplete");
    EXPECT_FALSE(type_is_complete(u));
    EXPECT_EQ(u->size, 0);
}

TEST(records, enum_is_int)
{
    Type *e = type_enum("P7Enum");
    EXPECT_TRUE(type_is_enum(e));
    EXPECT_TRUE(type_is_integer(e));
    EXPECT_TRUE(type_is_signed_int(e));
    EXPECT_FALSE(type_is_unsigned(e));
    EXPECT_EQ(type_sizeof(e), 4);
    EXPECT_EQ(type_rank(e), type_rank(type_int()));
    EXPECT_EQ(type_promote(e), type_int());
}

TEST(records, enum_constant_value)
{
    Arena *arena = arena_new();
    ASTNode *ast = tc_parse("enum Color { RED, GREEN = 5, BLUE };\n", arena);
    EXPECT_NOTNULL(ast);
    ASTProgram *prog = ast_as(ASTProgram, ast);
    ASTNode *decl = (ASTNode *) vec_get(prog->decls, 0);
    EXPECT_TRUE(decl->kind == AST_ENUM_DECL);
    ASTEnumDecl *ed = ast_as(ASTEnumDecl, decl);
    EXPECT_STR_EQ(ed->tag, "Color");
    EXPECT_EQ(vec_size(ed->constants), 3);
    EnumConstant *c0 = (EnumConstant *) vec_get(ed->constants, 0);
    EnumConstant *c1 = (EnumConstant *) vec_get(ed->constants, 1);
    EnumConstant *c2 = (EnumConstant *) vec_get(ed->constants, 2);
    EXPECT_EQ(c0->value, 0);
    EXPECT_EQ(c1->value, 5);
    EXPECT_EQ(c2->value, 6);
    arena_free(arena);
}

TEST(records, interp_struct_member)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct Pt p;\n"
                            "    p.x = 3;\n"
                            "    p.y = 4;\n"
                            "    return p.x * 10 + p.y;\n"
                            "}\n"),
              34);
}

TEST(records, interp_nested_member)
{
    EXPECT_EQ(tc_run_interp("struct In { int v; };\n"
                            "struct Out { struct In in; int w; };\n"
                            "int main(void) {\n"
                            "    struct Out o;\n"
                            "    o.in.v = 5;\n"
                            "    o.w = 10;\n"
                            "    return o.in.v + o.w;\n"
                            "}\n"),
              15);
}

TEST(records, interp_nested_struct_init)
{
    EXPECT_EQ(tc_run_interp("struct In { int x; int y; };\n"
                            "struct Out { struct In in; int z; };\n"
                            "int main(void) {\n"
                            "    struct Out o = {{4, 5}, 6};\n"
                            "    return o.in.x + o.in.y + o.z;\n"
                            "}\n"),
              15);
}

TEST(records, interp_struct_array_member)
{
    EXPECT_EQ(tc_run_interp("struct Rec { int a[3]; };\n"
                            "int main(void) {\n"
                            "    struct Rec r;\n"
                            "    r.a[0] = 1;\n"
                            "    r.a[1] = 2;\n"
                            "    r.a[2] = 3;\n"
                            "    return r.a[0] + r.a[1] + r.a[2];\n"
                            "}\n"),
              6);
}

TEST(records, interp_arrow)
{
    EXPECT_EQ(tc_run_interp("struct Node { int val; struct Node *next; };\n"
                            "int main(void) {\n"
                            "    struct Node a;\n"
                            "    struct Node b;\n"
                            "    a.val = 1;\n"
                            "    b.val = 2;\n"
                            "    a.next = &b;\n"
                            "    return a.val + a.next->val;\n"
                            "}\n"),
              3);
}

TEST(records, interp_struct_assign)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct Pt a;\n"
                            "    struct Pt b;\n"
                            "    a.x = 1;\n"
                            "    a.y = 2;\n"
                            "    b = a;\n"
                            "    b.x = 100;\n"
                            "    return a.x + a.y;\n"
                            "}\n"),
              3);
}

TEST(records, interp_deref_struct_assign)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct Pt a;\n"
                            "    struct Pt b;\n"
                            "    struct Pt *p = &a;\n"
                            "    struct Pt *q = &b;\n"
                            "    q->x = 7;\n"
                            "    q->y = 8;\n"
                            "    *p = *q;\n"
                            "    return p->x + p->y;\n"
                            "}\n"),
              15);
}

TEST(records, interp_struct_byval_arg)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int mut(struct Pt p) {\n"
                            "    p.x = p.x + 1;\n"
                            "    p.y = p.y + 1;\n"
                            "    return p.x + p.y;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    struct Pt a;\n"
                            "    a.x = 10;\n"
                            "    a.y = 20;\n"
                            "    int r = mut(a);\n"
                            "    return a.x + a.y + r;\n"
                            "}\n"),
              62);
}

TEST(records, interp_struct_return)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "struct Pt make(int a, int b) {\n"
                            "    struct Pt p;\n"
                            "    p.x = a;\n"
                            "    p.y = b;\n"
                            "    return p;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    struct Pt p = make(7, 8);\n"
                            "    return p.x * 10 + p.y;\n"
                            "}\n"),
              78);
}

TEST(records, interp_struct_array)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct Pt arr[3];\n"
                            "    arr[0].x = 1;\n"
                            "    arr[0].y = 2;\n"
                            "    arr[1].x = 3;\n"
                            "    arr[1].y = 4;\n"
                            "    arr[2].x = 5;\n"
                            "    arr[2].y = 6;\n"
                            "    return arr[0].x + arr[1].x + arr[2].x;\n"
                            "}\n"),
              9);
}

TEST(records, interp_arrow_loop)
{
    EXPECT_EQ(tc_run_interp("struct Cell { int v; };\n"
                            "int main(void) {\n"
                            "    struct Cell cells[5];\n"
                            "    int i;\n"
                            "    for (i = 0; i < 5; i = i + 1) {\n"
                            "        cells[i].v = i * 2;\n"
                            "    }\n"
                            "    struct Cell *p = &cells[3];\n"
                            "    return p->v;\n"
                            "}\n"),
              6);
}

TEST(records, interp_deref_addr_record)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "int main(void) {\n"
                            "    struct Pt q;\n"
                            "    q.x = 42;\n"
                            "    q.y = 7;\n"
                            "    return (*&q).y;\n"
                            "}\n"),
              7);
}

TEST(records, interp_union_pun)
{
    EXPECT_EQ(tc_run_interp("union Mix { int i; char c; };\n"
                            "int main(void) {\n"
                            "    union Mix m;\n"
                            "    m.i = 65;\n"
                            "    return m.c;\n"
                            "}\n"),
              65);
}

TEST(records, interp_union_pun_word)
{
    EXPECT_EQ(tc_run_interp("union Mix { int i; char c; };\n"
                            "int main(void) {\n"
                            "    union Mix m;\n"
                            "    m.i = 0x1234;\n"
                            "    return m.c;\n"
                            "}\n"),
              52);
}

TEST(records, interp_union_struct_member)
{
    EXPECT_EQ(tc_run_interp("struct Pt { int x; int y; };\n"
                            "union U { int i; struct Pt p; };\n"
                            "int main(void) {\n"
                            "    union U u;\n"
                            "    u.p.x = 3;\n"
                            "    u.p.y = 4;\n"
                            "    return u.p.x * 10 + u.p.y;\n"
                            "}\n"),
              34);
}

TEST(records, interp_union_in_struct)
{
    EXPECT_EQ(tc_run_interp("union U { int i; char c; };\n"
                            "struct S { char tag; union U u; int n; };\n"
                            "int main(void) {\n"
                            "    struct S s;\n"
                            "    s.u.i = 7;\n"
                            "    s.n = 10;\n"
                            "    return s.u.i + s.n;\n"
                            "}\n"),
              17);
}

TEST(records, interp_union_assign)
{
    EXPECT_EQ(tc_run_interp("union U { int i; char c; };\n"
                            "int main(void) {\n"
                            "    union U a;\n"
                            "    union U b;\n"
                            "    a.i = 42;\n"
                            "    b = a;\n"
                            "    b.c = 7;\n"
                            "    return a.i + b.i;\n"
                            "}\n"),
              49);
}

TEST(records, interp_union_byval_arg)
{
    EXPECT_EQ(tc_run_interp("union U { int i; char c; };\n"
                            "int mut(union U u) {\n"
                            "    u.i = u.i + 1;\n"
                            "    return u.i;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    union U a;\n"
                            "    a.i = 10;\n"
                            "    int r = mut(a);\n"
                            "    return a.i + r;\n"
                            "}\n"),
              21);
}

TEST(records, interp_union_return)
{
    EXPECT_EQ(tc_run_interp("union U { int i; char c; };\n"
                            "union U make(int v) {\n"
                            "    union U u;\n"
                            "    u.i = v;\n"
                            "    return u;\n"
                            "}\n"
                            "int main(void) {\n"
                            "    union U u = make(9);\n"
                            "    return u.c;\n"
                            "}\n"),
              9);
}

TEST(records, interp_union_array)
{
    EXPECT_EQ(tc_run_interp("union U { int i; char c; };\n"
                            "int main(void) {\n"
                            "    union U arr[4];\n"
                            "    int k;\n"
                            "    for (k = 0; k < 4; k = k + 1) {\n"
                            "        arr[k].i = k * 10;\n"
                            "    }\n"
                            "    return arr[0].i + arr[3].i;\n"
                            "}\n"),
              30);
}

TEST(records, interp_enum_const)
{
    EXPECT_EQ(tc_run_interp("enum { A = 3, B };\n"
                            "int main(void) {\n"
                            "    return A + B;\n"
                            "}\n"),
              7);
}

TEST(records, interp_enum_auto)
{
    EXPECT_EQ(tc_run_interp("enum { A, B, C = 10, D };\n"
                            "int main(void) {\n"
                            "    return A * 100 + B * 10 + C + D;\n"
                            "}\n"),
              31);
}

TEST(records, interp_enum_expr_const)
{
    EXPECT_EQ(tc_run_interp("enum { A = 1 << 4, B = A * 2 + 1, C = -B };\n"
                            "int main(void) {\n"
                            "    return A + B + C;\n"
                            "}\n"),
              16);
}

TEST(records, interp_enum_typed_var)
{
    EXPECT_EQ(tc_run_interp("enum Color { RED, GREEN, BLUE };\n"
                            "int main(void) {\n"
                            "    enum Color c = BLUE;\n"
                            "    return c;\n"
                            "}\n"),
              2);
}

TEST(records, interp_enum_member)
{
    EXPECT_EQ(tc_run_interp("enum Color { RED, GREEN };\n"
                            "struct Pt { int x; enum Color c; };\n"
                            "int main(void) {\n"
                            "    struct Pt s;\n"
                            "    s.x = 1;\n"
                            "    s.c = GREEN;\n"
                            "    return sizeof(struct Pt) + s.c;\n"
                            "}\n"),
              9);
}

TEST(records, elf_struct_member)
{
    EXPECT_EQ(tc_run_elf("struct Pt { int x; int y; };\n"
                         "int main(void) {\n"
                         "    struct Pt p;\n"
                         "    p.x = 3;\n"
                         "    p.y = 4;\n"
                         "    return p.x * 10 + p.y;\n"
                         "}\n"),
              34);
}

TEST(records, elf_struct_padding)
{
    EXPECT_EQ(tc_run_elf("struct Pair { char a; int b; };\n"
                         "int sum(struct Pair p) {\n"
                         "    return p.a + p.b;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    struct Pair p;\n"
                         "    p.a = 5;\n"
                         "    p.b = 100;\n"
                         "    return sum(p);\n"
                         "}\n"),
              105);
}

TEST(records, elf_struct_pointer)
{
    EXPECT_EQ(tc_run_elf("struct Node { int val; struct Node *next; };\n"
                         "int main(void) {\n"
                         "    struct Node a;\n"
                         "    struct Node b;\n"
                         "    a.val = 1;\n"
                         "    b.val = 2;\n"
                         "    a.next = &b;\n"
                         "    return a.val + a.next->val;\n"
                         "}\n"),
              3);
}

TEST(records, elf_struct_byval)
{
    EXPECT_EQ(tc_run_elf("struct Pt { int x; int y; };\n"
                         "struct Pt bump(struct Pt p) {\n"
                         "    p.x = p.x + 1;\n"
                         "    p.y = p.y + 1;\n"
                         "    return p;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    struct Pt a;\n"
                         "    a.x = 10;\n"
                         "    a.y = 20;\n"
                         "    struct Pt b = bump(a);\n"
                         "    return a.x + a.y + b.x + b.y;\n"
                         "}\n"),
              62);
}

TEST(records, elf_struct_return)
{
    EXPECT_EQ(tc_run_elf("struct Pt { int x; int y; };\n"
                         "struct Pt make(int a, int b) {\n"
                         "    struct Pt p;\n"
                         "    p.x = a;\n"
                         "    p.y = b;\n"
                         "    return p;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    struct Pt p = make(7, 8);\n"
                         "    return p.x * 10 + p.y;\n"
                         "}\n"),
              78);
}

TEST(records, elf_struct_array)
{
    EXPECT_EQ(tc_run_elf("struct Pt { int x; int y; };\n"
                         "int main(void) {\n"
                         "    struct Pt arr[3];\n"
                         "    arr[0].x = 1;\n"
                         "    arr[0].y = 2;\n"
                         "    arr[1].x = 3;\n"
                         "    arr[1].y = 4;\n"
                         "    arr[2].x = 5;\n"
                         "    arr[2].y = 6;\n"
                         "    return arr[0].x + arr[1].x + arr[2].x;\n"
                         "}\n"),
              9);
}

TEST(records, elf_union_pun)
{
    EXPECT_EQ(tc_run_elf("union Mix { int i; char c; };\n"
                         "int main(void) {\n"
                         "    union Mix m;\n"
                         "    m.i = 65;\n"
                         "    return m.c;\n"
                         "}\n"),
              65);
}

TEST(records, elf_union_byval)
{
    EXPECT_EQ(tc_run_elf("union U { int i; char c; };\n"
                         "union U make(int v) {\n"
                         "    union U u;\n"
                         "    u.i = v;\n"
                         "    return u;\n"
                         "}\n"
                         "int main(void) {\n"
                         "    union U u = make(9);\n"
                         "    return u.c;\n"
                         "}\n"),
              9);
}

TEST(records, elf_enum)
{
    EXPECT_EQ(tc_run_elf("enum { A = 3, B };\n"
                         "int main(void) {\n"
                         "    return A + B;\n"
                         "}\n"),
              7);
}

TEST(records, negative_unknown_member)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt p;\n"
                      "    return p.y;\n"
                      "}\n");
}

TEST(records, negative_dot_on_non_record)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x;\n"
                      "    return x.y;\n"
                      "}\n");
}

TEST(records, negative_arrow_on_non_pointer)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt p;\n"
                      "    return p->x;\n"
                      "}\n");
}

TEST(records, negative_sizeof_incomplete)
{
    EXPECT_BUILD_FAIL("struct Pt;\n"
                      "int main(void) {\n"
                      "    return sizeof(struct Pt);\n"
                      "}\n");
}

TEST(records, negative_redefined_tag)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "struct Pt { int y; };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_arith_on_record)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt a;\n"
                      "    struct Pt b;\n"
                      "    return a + b;\n"
                      "}\n");
}

TEST(records, negative_unary_on_record)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt p;\n"
                      "    return -p;\n"
                      "}\n");
}

TEST(records, negative_ternary_on_record)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "struct Q { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt a;\n"
                      "    struct Q b;\n"
                      "    return (1 ? a : b).x;\n"
                      "}\n");
}

TEST(records, ternary_same_record_ok)
{
    EXPECT_INTERP_AND_ELF("struct Pt { int x; int y; };\n"
                          "int main(int argc, char **argv) {\n"
                          "    struct Pt a = {1, 2};\n"
                          "    struct Pt b = {3, 4};\n"
                          "    struct Pt c = argc > 1 ? a : b;\n"
                          "    return c.x * 10 + c.y;\n"
                          "}\n",
                          34);
}

TEST(records, negative_record_init_scalar)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "int main(void) {\n"
                      "    struct Pt p = 5;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_record_return_scalar)
{
    EXPECT_BUILD_FAIL("struct Pt { int x; };\n"
                      "struct Pt f(void) {\n"
                      "    return 5;\n"
                      "}\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_unknown_member_union)
{
    EXPECT_BUILD_FAIL("union U { int i; };\n"
                      "int main(void) {\n"
                      "    union U u;\n"
                      "    return u.x;\n"
                      "}\n");
}

TEST(records, negative_tag_kind_collision)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "union S { int y; };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_sizeof_incomplete_union)
{
    EXPECT_BUILD_FAIL("union U;\n"
                      "int main(void) {\n"
                      "    return sizeof(union U);\n"
                      "}\n");
}

TEST(records, negative_enum_nonconst_init)
{
    EXPECT_BUILD_FAIL("enum { A = main() };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_enum_redefinition)
{
    EXPECT_BUILD_FAIL("enum E { A };\n"
                      "enum E { B };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_enum_tag_collision)
{
    EXPECT_BUILD_FAIL("struct S { int x; };\n"
                      "enum S { A };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_enum_const_shadow)
{
    EXPECT_BUILD_FAIL("enum { A };\n"
                      "int main(void) {\n"
                      "    int A = 1;\n"
                      "    return A;\n"
                      "}\n");
}

TEST(records, negative_enum_duplicate_const)
{
    EXPECT_BUILD_FAIL("enum { A, A };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, enum_value_uint_max_ok)
{
    EXPECT_BUILD_SUCCEED("enum { A = 4294967295u };\n"
                         "int main(void) {\n"
                         "    return 0;\n"
                         "}\n");
}

TEST(records, negative_enum_value_overflow)
{
    EXPECT_BUILD_FAIL("enum { A = 4294967296 };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_enum_incomplete)
{
    EXPECT_BUILD_FAIL("enum E;\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_enum_div_zero)
{
    EXPECT_BUILD_FAIL("enum { A = 1 / 0 };\n"
                      "int main(void) {\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, anonymous_union_members)
{
    /* C11 §6.7.2.1p13: anonymous union members share the enclosing layout. */
    EXPECT_INTERP_AND_ELF("struct V {\n"
                          "    int tag;\n"
                          "    union {\n"
                          "        struct { int i; } as_int;\n"
                          "        struct { char c; int n; } as_pair;\n"
                          "    };\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct V v;\n"
                          "    v.tag = 1;\n"
                          "    v.as_pair.n = 41;\n"
                          "    v.as_int.i = 1;\n"
                          "    return v.tag + v.as_pair.n + v.as_int.i - 1;\n"
                          "}\n",
                          42);
}

TEST(records, anonymous_struct_member)
{
    /* C11 §6.7.2.1p13: anonymous struct members share the enclosing layout. */
    EXPECT_INTERP_AND_ELF("struct S {\n"
                          "    int tag;\n"
                          "    struct { int x; int y; };\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.tag = 1;\n"
                          "    s.x = 2;\n"
                          "    s.y = 3;\n"
                          "    return s.tag + s.x + s.y;\n"
                          "}\n",
                          6);
}

TEST(records, anonymous_struct_in_union_layout)
{
    EXPECT_INTERP_AND_ELF("typedef unsigned long u64;\n"
                          "typedef struct Type Type;\n"
                          "struct Type {\n"
                          "    u64 a;\n"
                          "    union {\n"
                          "        struct { Type *pointee; } ptr;\n"
                          "        struct { Type *elem; u64 length; } arr;\n"
                          "    };\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct Type t;\n"
                          "    t.a = 1;\n"
                          "    t.arr.elem = &t;\n"
                          "    t.arr.length = 2;\n"
                          "    t.ptr.pointee = &t;\n"
                          "    return (int) (t.a + t.arr.length + (t.arr.elem == t.ptr.pointee));\n"
                          "}\n",
                          4);
}

TEST(records, anonymous_member_offset_query)
{
    Arena *a = arena_new();
    Type *inner = type_record_anon(TYPE_UNION);
    Vec *ifields = vec_new(a);
    vec_push(ifields, make_field(a, "x", type_int()));
    vec_push(ifields, make_field(a, "y", type_char()));
    type_record_complete(inner, ifields);

    Type *outer = type_record(TYPE_STRUCT, "P17AnonOuter");
    Vec *ofields = vec_new(a);
    vec_push(ofields, make_field(a, "head", type_long()));
    RecordField *anon = make_field(a, NULL, inner);
    vec_push(ofields, anon);
    vec_push(ofields, make_field(a, "tail", type_int()));
    type_record_complete(outer, ofields);

    EXPECT_EQ(anon->offset, 8);
    EXPECT_EQ(type_record_field_offset(outer, "x"), 8);
    EXPECT_EQ(type_record_field_offset(outer, "y"), 8);
    EXPECT_EQ(type_record_field_offset(outer, "tail"), 12);
    EXPECT_EQ(type_record_field(outer, "x"), type_int());
    EXPECT_EQ(outer->size, 16);
    arena_free(a);
}

TEST(records, many_same_shape_function_types_do_not_conflict)
{
    EXPECT_BUILD_SUCCEED("typedef struct N N;\n"
                         "struct N { int x; };\n"
                         "N *f1(N *p);\n"
                         "N *f2(N *p);\n"
                         "N *f3(N *p);\n"
                         "N *f1(N *p) { return p; }\n"
                         "N *f2(N *p) { return p; }\n"
                         "N *f3(N *p) { return p; }\n");
}

TEST(records, bitfield_layout_pack_ordinary)
{
    /* C11 §6.7.2.1: an ordinary member breaks a bit-field's storage unit. */
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P17BitPack");
    Vec *fields = vec_new(a);
    RecordField *a3 = make_field(a, "a", type_int());
    a3->bit_width = 3;
    RecordField *b = make_field(a, "b", type_int());
    RecordField *c5 = make_field(a, "c", type_int());
    c5->bit_width = 5;
    vec_push(fields, a3);
    vec_push(fields, b);
    vec_push(fields, c5);
    type_record_complete(s, fields);
    EXPECT_EQ(s->size, 12);
    EXPECT_EQ(s->align, 4);
    EXPECT_EQ(a3->offset, 0);
    EXPECT_EQ((u32) a3->bit_offset, 0);
    EXPECT_EQ(b->offset, 4);
    EXPECT_EQ(c5->offset, 8);
    EXPECT_EQ((u32) c5->bit_offset, 0);
    arena_free(a);
}

TEST(records, bitfield_layout_unit_after_char)
{
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P17BitAfterChar");
    Vec *fields = vec_new(a);
    RecordField *c = make_field(a, "c", type_char());
    RecordField *x = make_field(a, "x", type_int());
    x->bit_width = 1;
    vec_push(fields, c);
    vec_push(fields, x);
    type_record_complete(s, fields);
    EXPECT_EQ(s->size, 4);
    EXPECT_EQ(s->align, 4);
    EXPECT_EQ(c->offset, 0);
    EXPECT_EQ(x->offset, 0);
    EXPECT_EQ((u32) x->bit_offset, 8);
    arena_free(a);
}

TEST(records, bitfield_layout_split_storage_unit)
{
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P17BitSplit");
    Vec *fields = vec_new(a);
    RecordField *a1 = make_field(a, "a", type_int());
    a1->bit_width = 1;
    RecordField *b31 = make_field(a, "b", type_int());
    b31->bit_width = 31;
    RecordField *c1 = make_field(a, "c", type_int());
    c1->bit_width = 1;
    vec_push(fields, a1);
    vec_push(fields, b31);
    vec_push(fields, c1);
    type_record_complete(s, fields);
    EXPECT_EQ(s->size, 8);
    EXPECT_EQ(s->align, 4);
    EXPECT_EQ(a1->offset, 0);
    EXPECT_EQ((u32) a1->bit_offset, 0);
    EXPECT_EQ((u32) b31->bit_offset, 1);
    EXPECT_EQ(c1->offset, 4);
    EXPECT_EQ((u32) c1->bit_offset, 0);
    arena_free(a);
}

TEST(records, bitfield_layout_mixed_bases)
{
    Arena *a = arena_new();
    Type *s = type_record(TYPE_STRUCT, "P17BitMixed");
    Vec *fields = vec_new(a);
    RecordField *u1 = make_field(a, "is_unsigned", type_cbool());
    u1->bit_width = 1;
    RecordField *len = make_field(a, "length", type_enum("P17BFSuffix"));
    len->bit_width = 2;
    RecordField *h1 = make_field(a, "is_hex", type_cbool());
    h1->bit_width = 1;
    vec_push(fields, u1);
    vec_push(fields, len);
    vec_push(fields, h1);
    type_record_complete(s, fields);
    EXPECT_EQ(s->size, 4);
    EXPECT_EQ(s->align, 4);
    EXPECT_EQ(u1->offset, 0);
    EXPECT_EQ((u32) u1->bit_offset, 0);
    EXPECT_EQ((u32) len->bit_offset, 1);
    EXPECT_EQ((u32) h1->bit_offset, 3);
    u32 bo = 0;
    u32 bw = 0;
    EXPECT_TRUE(type_record_field_bit(s, "length", &bo, &bw));
    EXPECT_EQ(bo, 1);
    EXPECT_EQ(bw, 2);
    arena_free(a);
}

TEST(records, bitfield_signed_roundtrip)
{
    EXPECT_INTERP_AND_ELF("struct S {\n"
                          "    int a:3;\n"
                          "    int b;\n"
                          "    int c:5;\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.b = 0;\n"
                          "    s.a = 7;\n"
                          "    s.c = 15;\n"
                          "    int r = s.a + s.c;\n"
                          "    s.a = -4;\n"
                          "    r += s.a;\n"
                          "    s.c = -16;\n"
                          "    r += s.c;\n"
                          "    s.a = 0;\n"
                          "    s.c = 0;\n"
                          "    r += (int) sizeof(struct S);\n"
                          "    return r;\n"
                          "}\n",
                          6);
}

TEST(records, bitfield_unsigned_overflow_wraps)
{
    EXPECT_INTERP_AND_ELF("typedef _Bool bool;\n"
                          "struct F {\n"
                          "    bool f:1;\n"
                          "    unsigned n:3;\n"
                          "    bool g:1;\n"
                          "    int z;\n"
                          "};\n"
                          "int main(void) {\n"
                          "    struct F x;\n"
                          "    x.f = 1;\n"
                          "    x.n = 5;\n"
                          "    x.g = 0;\n"
                          "    x.z = 11;\n"
                          "    int r = (int) x.f + (int) x.n + (int) x.g + x.z;\n"
                          "    x.n = 99;\n"
                          "    r += (int) x.n;\n"
                          "    x.f = 0;\n"
                          "    r += (int) x.f;\n"
                          "    return r;\n"
                          "}\n",
                          20);
}

TEST(records, bitfield_padding_and_named_members)
{
    EXPECT_INTERP_AND_ELF("struct S { int a:3; int :5; int b:4; };\n"
                          "int main(void) {\n"
                          "    struct S s;\n"
                          "    s.a = -1;\n"
                          "    s.b = 7;\n"
                          "    int r = s.a + s.b;\n"
                          "    r += (int) sizeof(struct S);\n"
                          "    return r;\n"
                          "}\n",
                          10);
}

TEST(records, bitfield_token_style_stats)
{
    EXPECT_INTERP_AND_ELF(
        "typedef _Bool bool;\n"
        "typedef enum { LEN_I, LEN_L, LEN_LL } Suffix;\n"
        "struct TokenLike {\n"
        "    union { long long i; void *p; } payload;\n"
        "    struct { bool is_unsigned:1; Suffix length:2; bool is_hex:1; } flags;\n"
        "    int str_len;\n"
        "};\n"
        "int main(void) {\n"
        "    struct TokenLike t;\n"
        "    t.flags.is_unsigned = 1;\n"
        "    t.flags.length = LEN_LL;\n"
        "    t.flags.is_hex = 1;\n"
        "    t.str_len = 7;\n"
        "    int r = (int) sizeof(struct TokenLike);\n"
        "    r += (int) t.flags.length * 10;\n"
        "    r += (int) t.flags.is_unsigned + (int) t.flags.is_hex;\n"
        "    r += t.str_len;\n"
        "    return r;\n"
        "}\n",
        45);
}

TEST(records, bitfield_arrow_access)
{
    EXPECT_INTERP_AND_ELF("struct Node { unsigned tag:2; int next_i:30; struct Node *next; };\n"
                          "int main(void) {\n"
                          "    struct Node a;\n"
                          "    struct Node b;\n"
                          "    a.tag = 1;\n"
                          "    a.next_i = -2;\n"
                          "    b.tag = 2;\n"
                          "    b.next_i = 3;\n"
                          "    b.next = 0;\n"
                          "    a.next = &b;\n"
                          "    return a.next->tag * 10 + a.tag + a.next->next_i;\n"
                          "}\n",
                          2 * 10 + 1 + 3);
}

TEST(records, negative_bitfield_non_integer_type)
{
    EXPECT_BUILD_FAIL("struct S { char *p : 3; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(records, negative_bitfield_width_not_const)
{
    EXPECT_BUILD_FAIL("int g;\n"
                      "struct S { int x : g; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(records, negative_bitfield_width_exceeds_type)
{
    EXPECT_BUILD_FAIL("struct S { int x : 33; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(records, negative_bitfield_zero_width)
{
    EXPECT_BUILD_FAIL("struct S { int x : 0; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(records, negative_bitfield_bool_width)
{
    EXPECT_BUILD_FAIL("typedef _Bool bool;\n"
                      "struct S { bool b : 2; };\n"
                      "int main(void) { return 0; }\n");
}

TEST(records, negative_bitfield_take_address)
{
    EXPECT_BUILD_FAIL("struct S { int x : 3 };\n"
                      "int main(void) {\n"
                      "    struct S s;\n"
                      "    int *p = &s.x;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_incompatible_struct_assign)
{
    EXPECT_BUILD_FAIL("struct A { int x; };\n"
                      "struct B { int x; };\n"
                      "int main(void) {\n"
                      "    struct A a;\n"
                      "    struct B b;\n"
                      "    a = b;\n"
                      "    return 0;\n"
                      "}\n");
}

TEST(records, negative_struct_equality)
{
    EXPECT_BUILD_FAIL("struct A { int x; };\n"
                      "int main(void) {\n"
                      "    struct A a;\n"
                      "    struct A b;\n"
                      "    return a == b;\n"
                      "}\n");
}

TEST(records, negative_bitfield_negative_width)
{
    EXPECT_BUILD_FAIL("struct S { int x : -1; };\n"
                      "int main(void) { return 0; }\n");
}

static const char *bitfield_init_src =
    "struct S { unsigned a:1; unsigned b:2; unsigned c:1; unsigned d:3; };\n"
    "int main(void) {\n"
    "    struct S s = {1, 2, 1, 5};\n"
    "    if (s.a != 1 || s.b != 2 || s.c != 1 || s.d != 5) { return 1; }\n"
    "    struct S t = {.a = 1, .b = 3, .d = 4};\n"
    "    if (t.a != 1 || t.b != 3 || t.c != 0 || t.d != 4) { return 2; }\n"
    "    t.b = 6;\n"
    "    if (t.b != 2 || t.a != 1 || t.c != 0 || t.d != 4) { return 3; }\n"
    "    return 0;\n"
    "}\n";

TEST(records, bitfield_init)
{
    EXPECT_INTERP_AND_ELF(bitfield_init_src, 0);
}

static const char *union_payload_src =
    "typedef struct {\n"
    "    int kind;\n"
    "    long pad;\n"
    "    union { long int_val; const char *str; } payload;\n"
    "    unsigned sx:3;\n"
    "} Tok;\n"
    "int main(void) {\n"
    "    Tok t = {.payload = {.int_val = 0xFFFFFFFFFFFFFFFFUL}, .sx = 5};\n"
    "    if (t.payload.int_val != 0xFFFFFFFFFFFFFFFFUL) { return 1; }\n"
    "    if (t.sx != 5) { return 2; }\n"
    "    t.payload.int_val = 0xFFFF0000FFFF0000UL;\n"
    "    if (t.payload.int_val != 0xFFFF0000FFFF0000UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

TEST(records, union_payload_not_bitfield)
{
    EXPECT_INTERP_AND_ELF(union_payload_src, 0);
}

TEST(records, union_bitfield_share_base)
{
    EXPECT_INTERP_AND_ELF("union U { unsigned a:3; unsigned b:5; };\n"
                          "int main(void) {\n"
                          "    union U u;\n"
                          "    u.b = 5;\n"
                          "    return (int) u.a;\n"
                          "}\n",
                          5);
}

TEST(records, sizeof_member_array)
{
    /* §6.5.3.4p1: sizeof suppresses decay, so a member array keeps its extent. */
    EXPECT_INTERP_AND_ELF("struct S { char m[64]; int a[10]; };\n"
                          "struct S g;\n"
                          "int main(void) {\n"
                          "    struct S *p = &g;\n"
                          "    return (int)sizeof g.m + (int)sizeof(p->m)\n"
                          "         + (int)sizeof g.a + (int)sizeof(p->a) - 128;\n"
                          "}\n",
                          80);
}

TEST(records, copy16_round_trip)
{
    EXPECT_INTERP_AND_ELF("struct Quad { int a; int b; int c; int d; };\n"
                          "struct Quad make(int x)\n"
                          "{\n"
                          "    struct Quad q;\n"
                          "    q.a = x; q.b = x + 1; q.c = x + 2; q.d = x + 3;\n"
                          "    return q;\n"
                          "}\n"
                          "int main(void)\n"
                          "{\n"
                          "    struct Quad p = make(1);\n"
                          "    struct Quad q = p;\n"
                          "    struct Quad r = q;\n"
                          "    return r.a + r.b + r.c + r.d;\n"
                          "}\n",
                          10);
}

TEST(records, copy24_round_trip)
{
    EXPECT_INTERP_AND_ELF("struct S24 { int a, b, c, d, e, f; };\n"
                          "struct S24 make24(int x)\n"
                          "{\n"
                          "    struct S24 s;\n"
                          "    s.a = x; s.b = x + 1; s.c = x + 2;\n"
                          "    s.d = x + 3; s.e = x + 4; s.f = x + 5;\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void)\n"
                          "{\n"
                          "    struct S24 p = make24(10);\n"
                          "    struct S24 q = p;\n"
                          "    if (q.a != 10 || q.c != 12 || q.f != 15) { return 1; }\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}

TEST(records, copy32_round_trip)
{
    EXPECT_INTERP_AND_ELF("struct S32 { int a, b, c, d, e, f, g, h; };\n"
                          "struct S32 make32(int x)\n"
                          "{\n"
                          "    struct S32 s;\n"
                          "    s.a = x; s.b = x + 1; s.c = x + 2; s.d = x + 3;\n"
                          "    s.e = x + 4; s.f = x + 5; s.g = x + 6; s.h = x + 7;\n"
                          "    return s;\n"
                          "}\n"
                          "int main(void)\n"
                          "{\n"
                          "    struct S32 r = make32(20);\n"
                          "    struct S32 s = r;\n"
                          "    if (s.a != 20 || s.e != 24 || s.h != 27) { return 2; }\n"
                          "    return 0;\n"
                          "}\n",
                          0);
}
