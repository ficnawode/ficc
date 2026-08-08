#include "type.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/hashmap.h"

/* Integer type singletons. LP64 data model:
   char=8, short=16, int=32, long=64, long long=64. */
static const Type the_void = {.kind = TYPE_VOID, .width = 0, .align = 1, .size = 0};
static const Type the_bool = {.kind = TYPE_BOOL, .width = 8, .align = 1, .size = 1};
static const Type the_char = {.kind = TYPE_CHAR, .width = 8, .align = 1, .size = 1};
static const Type the_short = {.kind = TYPE_SHORT, .width = 16, .align = 2, .size = 2};
static const Type the_int = {.kind = TYPE_INT, .width = 32, .align = 4, .size = 4};
static const Type the_long = {.kind = TYPE_LONG, .width = 64, .align = 8, .size = 8};
static const Type the_llong = {.kind = TYPE_LLONG, .width = 64, .align = 8, .size = 8};
static const Type the_uchar = {.kind = TYPE_UCHAR, .width = 8, .align = 1, .size = 1};
static const Type the_ushort = {.kind = TYPE_USHORT, .width = 16, .align = 2, .size = 2};
static const Type the_uint = {.kind = TYPE_UINT, .width = 32, .align = 4, .size = 4};
static const Type the_ulong = {.kind = TYPE_ULONG, .width = 64, .align = 8, .size = 8};
static const Type the_ullong = {.kind = TYPE_ULLONG, .width = 64, .align = 8, .size = 8};

/* Composite type intern pool */
static Arena *type_arena;
static U64Map *ptr_cache;
static U64Map *array_cache;

Type *type_void(void)
{
    return (Type *) &the_void;
}
Type *type_cbool(void)
{
    return (Type *) &the_bool;
} /* C11 _Bool */
Type *type_char(void)
{
    return (Type *) &the_char;
}
Type *type_short(void)
{
    return (Type *) &the_short;
}
Type *type_int(void)
{
    return (Type *) &the_int;
}
Type *type_long(void)
{
    return (Type *) &the_long;
}
Type *type_llong(void)
{
    return (Type *) &the_llong;
}
Type *type_uchar(void)
{
    return (Type *) &the_uchar;
}
Type *type_ushort(void)
{
    return (Type *) &the_ushort;
}
Type *type_uint(void)
{
    return (Type *) &the_uint;
}
Type *type_ulong(void)
{
    return (Type *) &the_ulong;
}
Type *type_ullong(void)
{
    return (Type *) &the_ullong;
}

bool type_is_signed(Type *t)
{
    return t->kind == TYPE_BOOL || t->kind == TYPE_CHAR || t->kind == TYPE_SHORT ||
           t->kind == TYPE_INT || t->kind == TYPE_LONG || t->kind == TYPE_LLONG;
}

bool type_is_unsigned(Type *t)
{
    return t->kind == TYPE_UCHAR || t->kind == TYPE_USHORT || t->kind == TYPE_UINT ||
           t->kind == TYPE_ULONG || t->kind == TYPE_ULLONG;
}

bool type_is_integer(Type *t)
{
    return type_is_signed(t) || type_is_unsigned(t);
}

int type_rank(Type *t)
{
    switch (t->kind)
    {
        case TYPE_BOOL:
            return 0;
        case TYPE_CHAR:
        case TYPE_UCHAR:
            return 1;
        case TYPE_SHORT:
        case TYPE_USHORT:
            return 2;
        case TYPE_INT:
        case TYPE_UINT:
            return 3;
        case TYPE_LONG:
        case TYPE_ULONG:
            return 4;
        case TYPE_LLONG:
        case TYPE_ULLONG:
            return 5;
        default:
            return -1;
    }
}

Type *type_promote(Type *t)
{
    /* C11 §6.3.1.1: if an int can represent all values of the original type,
       the value is converted to int; otherwise unsigned int. */
    if (!type_is_integer(t))
    {
        return t;
    }
    if (type_rank(t) < type_rank(type_int()))
    {
        return type_int();
    }
    return t;
}

Type *type_common(Type *a, Type *b)
{
    ASSERT(type_is_integer(a) && type_is_integer(b));

    a = type_promote(a);
    b = type_promote(b);

    if (a->kind == b->kind)
    {
        return a;
    }

    bool a_signed = type_is_signed(a);
    bool b_signed = type_is_signed(b);
    int a_rank = type_rank(a);
    int b_rank = type_rank(b);

    if (a_signed == b_signed)
    {
        return a_rank >= b_rank ? a : b;
    }

    /* Mixed signedness: if unsigned rank >= signed rank, result is unsigned.
       Otherwise result is the signed type (it has greater rank). */
    Type *signed_type = a_signed ? a : b;
    Type *unsigned_type = a_signed ? b : a;

    if (type_rank(unsigned_type) >= type_rank(signed_type))
    {
        return unsigned_type;
    }
    return signed_type;
}

static Type *fit_hex(u64 uval, bool has_ullong)
{
    if (uval <= (u64) INT32_MAX)
    {
        return type_int();
    }
    if (uval <= (u64) UINT32_MAX)
    {
        return type_uint();
    }
    if (uval <= (u64) INT64_MAX)
    {
        return type_long();
    }
    if (has_ullong && uval <= (u64) UINT64_MAX)
    {
        return type_ulong();
    }
    return has_ullong ? type_ullong() : type_ulong();
}

static Type *fit_none(u64 uval, IntSuffix length)
{
    if (uval <= (u64) INT32_MAX)
    {
        return type_int();
    }
    if (length == SUFFIX_L)
    {
        return type_long();
    }
    return type_llong();
}

Type *type_int_literal(i64 value, bool is_hex, bool is_unsigned, IntSuffix length)
{
    u64 uval = (u64) value;

    if (length == SUFFIX_LL)
    {
        return is_unsigned ? type_ullong() : type_llong();
    }
    if (length == SUFFIX_L && is_unsigned)
    {
        return type_ulong();
    }
    if (is_unsigned || is_hex)
    {
        return fit_hex(uval, /* has_ullong */ length == SUFFIX_NONE);
    }
    return fit_none(uval, length);
}

static void type_init_pool(void)
{
    if (type_arena)
    {
        return;
    }
    type_arena = arena_new();
    ptr_cache = u64map_new(type_arena);
    array_cache = u64map_new(type_arena);
}

Type *type_ptr(Type *pointee)
{
    type_init_pool();
    u64 key = (u64) (uintptr_t) pointee;
    Type *cached = u64map_get(ptr_cache, key);
    if (cached)
    {
        return cached;
    }
    Type *t = arena_alloc(type_arena, sizeof(Type), _Alignof(Type));
    t->kind = TYPE_PTR;
    t->width = 64;
    t->align = 8;
    t->size = 8;
    t->ptr.pointee = pointee;
    u64map_set(ptr_cache, key, t);
    return t;
}

Type *type_array(Type *elem, u64 length)
{
    type_init_pool();
    u64 key = ((u64) (uintptr_t) elem) ^ (length << 3);
    Type *cached = u64map_get(array_cache, key);
    if (cached)
    {
        return cached;
    }
    Type *t = arena_alloc(type_arena, sizeof(Type), _Alignof(Type));
    t->kind = TYPE_ARRAY;
    t->width = elem->width;
    t->align = elem->align;
    t->size = (u32) (elem->size * length);
    t->arr.elem = elem;
    t->arr.length = length;
    u64map_set(array_cache, key, t);
    return t;
}

bool type_is_ptr(Type *t)
{
    return t->kind == TYPE_PTR;
}

bool type_is_array(Type *t)
{
    return t->kind == TYPE_ARRAY;
}

Type *type_deref(Type *t)
{
    ASSERT(t->kind == TYPE_PTR);
    return t->ptr.pointee;
}

Type *type_array_elem(Type *t)
{
    ASSERT(t->kind == TYPE_ARRAY);
    return t->arr.elem;
}

u64 type_array_len(Type *t)
{
    ASSERT(t->kind == TYPE_ARRAY);
    return t->arr.length;
}

Type *type_decay(Type *t)
{
    if (t->kind == TYPE_ARRAY)
    {
        return type_ptr(t->arr.elem);
    }
    return t;
}

u64 type_sizeof(Type *t)
{
    ASSERT(t->kind != TYPE_VOID);
    return t->size;
}

const char *type_kind_name(TypeKind kind)
{
    switch (kind)
    {
#define CASE(K)                                                                                    \
    case K:                                                                                        \
        return #K;
        TYPE_KINDS(CASE)
#undef CASE
    }
    return "TYPE_UNKNOWN";
}
