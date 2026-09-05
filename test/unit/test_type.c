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

TEST(type, va_list_shape)
{
    /* D15.2: `va_list` is glibc's x86-64 shape — an array of one 24-byte
       struct `{u32 gp_offset; u32 fp_offset; void *overflow; void *regs}`,
       decaying to a plain pointer on use. */
    Type *vl = type_va_list();
    EXPECT_EQ(vl->kind, TYPE_ARRAY);
    EXPECT_EQ(type_array_len(vl), 1);
    Type *e = type_array_elem(vl);
    EXPECT_TRUE(type_is_record(e));
    EXPECT_EQ(e->size, 24);
    EXPECT_EQ(e->align, 8);

    EXPECT_EQ(vec_size(e->record.fields), 4);
    RecordField *gp = (RecordField *) vec_get(e->record.fields, 0);
    EXPECT_EQ(gp->offset, 0);
    EXPECT_EQ(gp->type->size, 4);
    RecordField *fp = (RecordField *) vec_get(e->record.fields, 1);
    EXPECT_EQ(fp->offset, 4);
    RecordField *ovf = (RecordField *) vec_get(e->record.fields, 2);
    EXPECT_EQ(ovf->offset, 8);
    EXPECT_TRUE(type_is_ptr(ovf->type));
    RecordField *regs = (RecordField *) vec_get(e->record.fields, 3);
    EXPECT_EQ(regs->offset, 16);
    EXPECT_TRUE(type_is_ptr(regs->type));

    /* An identifier of type va_list decays to a pointer to the struct. */
    EXPECT_TRUE(type_decay(vl)->kind == TYPE_PTR);
    EXPECT_TRUE(type_deref(type_decay(vl)) == e);
}
/* --- Phase 16b (D16.1): pointer-to-function composition --- */

TEST(type, fn_ptr_is_ptr_of_interned_func)
{
    Arena *a = arena_new();
    Type *ft = type_func(type_int(), params_int(a, 1), false);
    Type *fp1 = type_ptr(ft);
    Type *fp2 = type_ptr(ft);
    /* Pointer interning: the same pointee is the same pointer type. */
    EXPECT_TRUE(fp1 == fp2);
    EXPECT_EQ(fp1->kind, TYPE_PTR);
    EXPECT_EQ(sizeof(void *), 8);
    EXPECT_EQ(type_sizeof(fp1), 8);
    EXPECT_EQ(type_deref(fp1), ft);
    arena_free(a);
}

TEST(type, fn_ptr_decay_from_designator)
{
    Arena *a = arena_new();
    Type *ft = type_func(type_int(), params_int(a, 1), false);
    /* A function designator decays to a pointer to the function (§6.3.2.1p4). */
    Type *decayed = type_decay(ft);
    EXPECT_EQ(decayed->kind, TYPE_PTR);
    EXPECT_EQ(type_deref(decayed), ft);
    /* Decay of a non-function type is identity. */
    EXPECT_TRUE(type_decay(type_int()) == type_int());
    arena_free(a);
}

TEST(type, type_is_function)
{
    Arena *a = arena_new();
    Type *ft = type_func(type_int(), params_int(a, 1), false);
    EXPECT_TRUE(type_is_function(ft));
    EXPECT_FALSE(type_is_function(type_ptr(ft)));
    EXPECT_FALSE(type_is_function(type_int()));
    arena_free(a);
}
