#include "harness.h"
#include "testdriver.h"
#include "type.h"

TEST(pointers, type_ptr_width)
{
    Type *p = type_ptr(type_int());
    EXPECT_EQ(p->width, 64);
    EXPECT_EQ(p->align, 8);
    EXPECT_EQ(p->size, 8);
}

TEST(pointers, type_ptr_interning)
{
    Type *a = type_ptr(type_int());
    Type *b = type_ptr(type_int());
    EXPECT_TRUE(a == b);
}

TEST(pointers, type_array_basic)
{
    Type *arr = type_array(type_int(), 10);
    EXPECT_EQ(arr->size, 40);
    EXPECT_EQ(type_array_elem(arr), type_int());
}

TEST(pointers, type_decay_deref)
{
    Type *p = type_ptr(type_char());
    EXPECT_EQ(type_deref(p), type_char());
}

TEST(pointers, interp_simple)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    return 42;\n"
                            "}\n"),
              42);
}

TEST(pointers, interp_sizeof_int)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    return sizeof(int);\n"
                            "}\n"),
              4);
}

TEST(pointers, interp_sizeof_char)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    return sizeof(char);\n"
                            "}\n"),
              1);
}

TEST(pointers, interp_sizeof_array_ident)
{
    /* Array-to-pointer decay is suppressed for the operand of sizeof (§6.3.2.1p3). */
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int a[10];\n"
                            "    return sizeof(a);\n"
                            "}\n"),
              40);
}

TEST(pointers, interp_string_literal)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    char *s = \"hello\";\n"
                            "    return s[0];\n"
                            "}\n"),
              'h');
}

TEST(pointers, interp_multiple_strings)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    char *a = \"foo\";\n"
                            "    char *b = \"bar\";\n"
                            "    return a[0] + a[1] + a[2] + b[0] + b[1] + b[2];\n"
                            "}\n"),
              'f' + 'o' + 'o' + 'b' + 'a' + 'r');
}

TEST(pointers, interp_array_subscript)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int arr[3];\n"
                            "    arr[0] = 10;\n"
                            "    arr[1] = 20;\n"
                            "    arr[2] = 30;\n"
                            "    return arr[0] + arr[1] + arr[2];\n"
                            "}\n"),
              60);
}

TEST(pointers, interp_array_write_read)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int arr[4];\n"
                            "    int i;\n"
                            "    for (i = 0; i < 4; i = i + 1) {\n"
                            "        arr[i] = i * 7;\n"
                            "    }\n"
                            "    return arr[3];\n"
                            "}\n"),
              21);
}

TEST(pointers, interp_ptr_arithmetic)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int arr[3];\n"
                            "    arr[0] = 5;\n"
                            "    arr[1] = 6;\n"
                            "    arr[2] = 7;\n"
                            "    int *p = arr;\n"
                            "    return *(p + 2);\n"
                            "}\n"),
              7);
}

TEST(pointers, interp_ptr_neg_offset)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int arr[3];\n"
                            "    arr[0] = 5;\n"
                            "    arr[1] = 6;\n"
                            "    arr[2] = 7;\n"
                            "    int *p = &arr[2];\n"
                            "    return *(p - 1);\n"
                            "}\n"),
              6);
}

TEST(pointers, interp_addr_deref)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int arr[2];\n"
                            "    arr[0] = 9;\n"
                            "    return *(&arr[0]);\n"
                            "}\n"),
              9);
}

TEST(pointers, interp_pointer_relational_compare)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int a[4] = {0, 1, 2, 3};\n"
                            "    int *p = a;\n"
                            "    int *q = a + 3;\n"
                            "    if (!(p < q)) return 1;\n"
                            "    if (!(q > p)) return 2;\n"
                            "    if (!(p <= q)) return 3;\n"
                            "    if (!(q >= p)) return 4;\n"
                            "    if (p >= q) return 5;\n"
                            "    return 0;\n"
                            "}\n"),
              0);
}

TEST(pointers, interp_pointer_equality)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int a[4] = {0, 1, 2, 3};\n"
                            "    int *p = a;\n"
                            "    int *q = a + 2;\n"
                            "    if (p == q) return 1;\n"
                            "    if (p != a) return 2;\n"
                            "    if (q != a + 2) return 3;\n"
                            "    return 0;\n"
                            "}\n"),
              0);
}

static const char *array_param_src = "int sum(int a[], int n) {\n"
                                     "    int s = 0;\n"
                                     "    int i;\n"
                                     "    for (i = 0; i < n; i = i + 1) {\n"
                                     "        s = s + a[i];\n"
                                     "    }\n"
                                     "    return s;\n"
                                     "}\n"
                                     "int main(void) {\n"
                                     "    int x[3];\n"
                                     "    x[0] = 1;\n"
                                     "    x[1] = 2;\n"
                                     "    x[2] = 3;\n"
                                     "    return sum(x, 3);\n"
                                     "}\n";

TEST(pointers, interp_array_param)
{
    EXPECT_EQ(tc_run_interp(array_param_src), 6);
}

TEST(pointers, interp_void_ptr_assign)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    void *v;\n"
                            "    int *p;\n"
                            "    v = p;\n"
                            "    p = v;\n"
                            "    return sizeof(v);\n"
                            "}\n"),
              8);
}

TEST(pointers, interp_string_pass_to_func)
{
    EXPECT_EQ(tc_run_interp("int first(char *s) {\n"
                            "    return s[0];\n"
                            "}\n"
                            "int main(void) {\n"
                            "    return first(\"hi\");\n"
                            "}\n"),
              'h');
}

TEST(pointers, interp_null_deref_trap)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    int *p = 0;\n"
                            "    *p = 42;\n"
                            "    return 0;\n"
                            "}\n"),
              1);
}

TEST(pointers, elf_golden_pointers)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    if (sizeof(int) != 4) {\n"
                         "        return 1;\n"
                         "    }\n"
                         "    if (sizeof(char) != 1) {\n"
                         "        return 2;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n"),
              0);
}

TEST(pointers, interp_string_loop_sum)
{
    EXPECT_EQ(tc_run_interp("int main(void) {\n"
                            "    char *s = \"hello\";\n"
                            "    int i = 0;\n"
                            "    int n = 0;\n"
                            "    while (s[i] != 0) {\n"
                            "        n = n + s[i];\n"
                            "        i = i + 1;\n"
                            "    }\n"
                            "    return n;\n"
                            "}\n"),
              'h' + 'e' + 'l' + 'l' + 'o');
}

TEST(pointers, elf_string_literal)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    char *s = \"ok\";\n"
                         "    if (s[0] != 111) {\n"
                         "        return 1;\n"
                         "    }\n"
                         "    if (s[1] != 107) {\n"
                         "        return 2;\n"
                         "    }\n"
                         "    return 42;\n"
                         "}\n"),
              42);
}

TEST(pointers, elf_array_read_write)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    int arr[3];\n"
                         "    arr[0] = 4;\n"
                         "    arr[1] = 5;\n"
                         "    arr[2] = 6;\n"
                         "    return arr[0] + arr[1] + arr[2];\n"
                         "}\n"),
              15);
}

TEST(pointers, elf_ptr_arithmetic)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    int arr[3];\n"
                         "    arr[0] = 10;\n"
                         "    arr[1] = 20;\n"
                         "    arr[2] = 30;\n"
                         "    int *p = arr;\n"
                         "    return *(p + 2);\n"
                         "}\n"),
              30);
}

TEST(pointers, elf_pointer_relational_compare)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    int a[4] = {0, 1, 2, 3};\n"
                         "    int *p = a;\n"
                         "    int *q = a + 3;\n"
                         "    if (!(p < q)) return 1;\n"
                         "    if (!(q > p)) return 2;\n"
                         "    if (!(p <= q)) return 3;\n"
                         "    if (!(q >= p)) return 4;\n"
                         "    if (p >= q) return 5;\n"
                         "    return 0;\n"
                         "}\n"),
              0);
}

TEST(pointers, elf_pointer_equality)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    int a[4] = {0, 1, 2, 3};\n"
                         "    int *p = a;\n"
                         "    int *q = a + 2;\n"
                         "    if (p == q) return 1;\n"
                         "    if (p != a) return 2;\n"
                         "    if (q != a + 2) return 3;\n"
                         "    return 0;\n"
                         "}\n"),
              0);
}

TEST(pointers, elf_array_param)
{
    EXPECT_EQ(tc_run_elf(array_param_src), 6);
}

TEST(pointers, elf_string_loop)
{
    EXPECT_EQ(tc_run_elf("int main(void) {\n"
                         "    char *s = \"hello\";\n"
                         "    int i = 0;\n"
                         "    while (s[i] != 0) {\n"
                         "        i = i + 1;\n"
                         "    }\n"
                         "    return i;\n"
                         "}\n"),
              5);
}

TEST(pointers, negative_deref_non_pointer)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int x;\n"
                      "    return *x;\n"
                      "}\n");
}

TEST(pointers, negative_addr_non_lvalue)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return &42;\n"
                      "}\n");
}

TEST(pointers, negative_sizeof_void)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    return sizeof(void);\n"
                      "}\n");
}

TEST(pointers, negative_incompatible_ptr_assign)
{
    EXPECT_BUILD_FAIL("int main(void) {\n"
                      "    int *p;\n"
                      "    char *q;\n"
                      "    p = q;\n"
                      "    return 0;\n"
                      "}\n");
}

/* void* parameters (C11 §6.7.6.3p10: only a lone `void` is the empty parameter-list marker) */

TEST(pointers, void_ptr_param_deref)
{
    EXPECT_INTERP_AND_ELF("int read_int(void *p) {\n"
                          "    return *(int *)p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 42;\n"
                          "    if (read_int(&x) != 42) return 1;\n"
                          "    int g = 9;\n"
                          "    if (read_int(&g) != 9) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(pointers, void_ptr_param_null_constant)
{
    EXPECT_INTERP_AND_ELF("int is_null(void *p) {\n"
                          "    return p == 0 ? 1 : 0;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    if (is_null(0) != 1) return 1;\n"
                          "    int x = 3;\n"
                          "    if (is_null(&x) != 0) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(pointers, void_ptr_multi_args)
{
    EXPECT_INTERP_AND_ELF("int sum(void *a, void *b) {\n"
                          "    return *(int *)a + *(int *)b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 40;\n"
                          "    int y = 2;\n"
                          "    if (sum(&x, &y) != 42) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(pointers, void_ptr_return)
{
    EXPECT_INTERP_AND_ELF("void *id(void *p) {\n"
                          "    return p;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 7;\n"
                          "    void *v = id(&x);\n"
                          "    if (*(int *)v != 7) return 1;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}

TEST(pointers, void_ptr_struct_member)
{
    EXPECT_INTERP_AND_ELF("struct box { void *p; int tag; };\n"
                          "struct box make(int *p) {\n"
                          "    struct box b;\n"
                          "    b.p = p;\n"
                          "    b.tag = 42;\n"
                          "    return b;\n"
                          "}\n"
                          "int main(void) {\n"
                          "    int x = 5;\n"
                          "    struct box b = make(&x);\n"
                          "    if (b.tag != 42) return 1;\n"
                          "    if (*(int *)b.p != 5) return 2;\n"
                          "    return 42;\n"
                          "}\n",
                          42);
}
