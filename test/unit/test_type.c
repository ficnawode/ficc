#include "harness.h"
#include "type.h"
#include "util/arena.h"
#include "util/vec.h"

/* Phase 15a (D15.1): the interned function Type. Equal signatures must be
   pointer-equal within a translation unit; the variadic bit and the parameter
   set are part of the identity; the layout is the pointer-face (align/size 8,
   width 0 — a function type has no value width). */

static Vec *params_int(Arena *a, size_t n)
{
    Vec *v = vec_new(a);
    for (size_t i = 0; i < n; i++)
    {
        vec_push(v, type_int());
    }
    return v;
}

TEST(type, func_type_interns_equal_signatures)
{
    Arena *a = arena_new();
    Type *f1 = type_func(type_int(), params_int(a, 2), false);
    Type *f2 = type_func(type_int(), params_int(a, 2), false);
    EXPECT_TRUE(f1 == f2);
    arena_free(a);
}

TEST(type, func_type_identity_has_variadic_bit)
{
    Arena *a = arena_new();
    Type *fixed = type_func(type_int(), params_int(a, 1), false);
    Type *variadic = type_func(type_int(), params_int(a, 1), true);
    EXPECT_TRUE(fixed != variadic);
    EXPECT_FALSE(type_is_variadic(fixed));
    EXPECT_TRUE(type_is_variadic(variadic));
    arena_free(a);
}

TEST(type, func_type_identity_has_param_set)
{
    Arena *a = arena_new();
    Vec *onepar = vec_new(a);
    vec_push(onepar, type_int());
    Vec *twopar = vec_new(a);
    vec_push(twopar, type_int());
    vec_push(twopar, type_int());
    Vec *longpar = vec_new(a);
    vec_push(longpar, type_long());
    EXPECT_TRUE(type_func(type_int(), onepar, false) != type_func(type_int(), twopar, false));
    EXPECT_TRUE(type_func(type_int(), onepar, false) != type_func(type_int(), longpar, false));
    arena_free(a);
}

TEST(type, func_type_identity_has_return_type)
{
    Arena *a = arena_new();
    Vec *vs = vec_new(a);
    vec_push(vs, type_int());
    EXPECT_TRUE(type_func(type_int(), vs, false) != type_func(type_void(), vs, false));
    arena_free(a);
}

TEST(type, func_type_layout)
{
    Arena *a = arena_new();
    Type *f = type_func(type_int(), params_int(a, 1), true);
    EXPECT_EQ(f->kind, TYPE_FUNC);
    EXPECT_EQ(f->width, 0);
    EXPECT_EQ(f->align, 8);
    EXPECT_EQ(f->size, 8);
    arena_free(a);
}