#include "type.h"
#include "util/arena.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include "util/vec.h"
#include <string.h>

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

/* MurmurHash3 finalizer (same avalanche as hash_u64): mixes the key so
   `elem`-pointer alignment and `length` low bits cannot alias. The naive
   `(u64)elem ^ (length << 3)` collides when elem pointers sit 2^k apart — the
   static singletons are spaced 128 bytes, so char[16] and int[0] share a key. */
static u64 type_key_mix(u64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Qualified-type variant cache: base Type* -> const-qualified variant.
   Scalars, pointers, records, and enums get one interned variant (idempotent,
   so `const const int` is legal C11); arrays are qualified through their
   element type and never appear as wrapper types here. For records the variant
   keeps a pointer back to the tag-intermed canonical (`unqual_base`), and the
   canonical keeps a list of its variants so completion propagates. */
static U64Map *qual_cache;

/* Tag namespace: tag string -> record/enum Type* (interning, pointer-equality). */
static Arena *tag_arena;
static StrMap *tag_table;

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
           t->kind == TYPE_INT || t->kind == TYPE_LONG || t->kind == TYPE_LLONG ||
           t->kind == TYPE_ENUM;
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

bool type_is_record(Type *t)
{
    return t->kind == TYPE_STRUCT || t->kind == TYPE_UNION;
}

bool type_is_struct(Type *t)
{
    return t->kind == TYPE_STRUCT;
}

bool type_is_union(Type *t)
{
    return t->kind == TYPE_UNION;
}

bool type_is_enum(Type *t)
{
    return t->kind == TYPE_ENUM;
}

bool type_is_complete(Type *t)
{
    if (type_is_array(t))
    {
        /* §6.7.6.2p4: a length-0 array is the `[]` "as unspecified" sentinel
           until an initializer completes it (D12.7) — incomplete. An array of
           incomplete elements is likewise incomplete. */
        if (t->arr.length == 0)
        {
            return false;
        }
        return type_is_complete(t->arr.elem);
    }
    if (!type_is_record(t))
    {
        return true;
    }
    return (t->unqual_base ? t->unqual_base : t)->record.complete;
}

bool type_is_const(Type *t)
{
    return (t->qualifiers & Q_CONST) != 0;
}

/* The tag-intermed unqualified record/enum a qualified variant wraps. */
static Type *type_base(Type *t)
{
    return t->unqual_base ? t->unqual_base : t;
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
        case TYPE_ENUM:
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
    t = type_unqual(t);
    if (!type_is_integer(t))
    {
        return t;
    }
    /* Enums are int-sized (D7); promote them to int proper. */
    if (t->kind == TYPE_ENUM || type_rank(t) < type_rank(type_int()))
    {
        return type_int();
    }
    return t;
}

Type *type_common(Type *a, Type *b)
{
    ASSERT(type_is_integer(a) && type_is_integer(b));

    /* Arithmetic results are unqualified rvalues (C11 §6.3.2.1). */
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
    qual_cache = u64map_new(type_arena);
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
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->ptr.pointee = pointee;
    u64map_set(ptr_cache, key, t);
    return t;
}

Type *type_array(Type *elem, u64 length)
{
    type_init_pool();
    u64 key = type_key_mix((u64) (uintptr_t) elem) ^ type_key_mix(length);
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
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->arr.elem = elem;
    t->arr.length = length;
    u64map_set(array_cache, key, t);
    return t;
}

Type *type_const(Type *t)
{
    if (t->qualifiers & Q_CONST)
    {
        return t;
    }
    /* Qualifying an array qualifies its element type (C11: `const int a[3]`
       is an array of const int). Decay then yields `const int*`, and `a[i]`
       lvalues are const through the element type. */
    if (t->kind == TYPE_ARRAY)
    {
        return type_array(type_const(t->arr.elem), t->arr.length);
    }
    type_init_pool();
    Type *cached = u64map_get(qual_cache, (u64) (uintptr_t) t);
    if (cached)
    {
        return cached;
    }
    Type *q = arena_alloc(type_arena, sizeof(Type), _Alignof(Type));
    *q = *t;
    q->qualifiers |= Q_CONST;
    q->unqual_base = t;
    if (type_is_record(q))
    {
        q->record.qual_variants = NULL;
        Type *base = type_base(t);
        if (!base->record.qual_variants)
        {
            base->record.qual_variants = vec_new(type_arena);
        }
        vec_push(base->record.qual_variants, q);
    }
    u64map_set(qual_cache, (u64) (uintptr_t) t, q);
    return q;
}

Type *type_unqual(Type *t)
{
    while (t && t->unqual_base)
    {
        t = t->unqual_base;
    }
    if (!t)
    {
        return NULL;
    }
    if (t->kind == TYPE_ARRAY)
    {
        Type *elem = type_unqual(t->arr.elem);
        if (elem == t->arr.elem)
        {
            return t;
        }
        return type_array(elem, t->arr.length);
    }
    return t;
}

Type *type_rvalue(Type *t)
{
    return type_unqual(t);
}

static void type_init_tags(void)
{
    if (tag_arena)
    {
        return;
    }
    tag_arena = arena_new();
    tag_table = strmap_new(tag_arena);
}

void type_reset(void)
{
    type_init_tags();
    tag_table = strmap_new(tag_arena);
}

/* Record/enum types are immortal (see type_reset above), so their tag strings
   — used both as `record.tag`/`enumm.tag` and as the tag_table keys — must not
   dangle into a per-compilation arena. Copy the caller's tag into the immortal
   tag_arena the first time it is stored. */
static const char *tag_intern(const char *tag)
{
    size_t len = strlen(tag);
    char *copy = arena_alloc(tag_arena, len + 1, sizeof(char));
    memcpy(copy, tag, len + 1);
    return copy;
}

Type *type_record(TypeKind kind, const char *tag)
{
    ASSERT(kind == TYPE_STRUCT || kind == TYPE_UNION);
    type_init_tags();

    Type *existing = strmap_get(tag_table, tag);
    if (existing)
    {
        return existing;
    }

    Type *t = arena_alloc(tag_arena, sizeof(Type), _Alignof(Type));
    t->kind = kind;
    t->width = 0;
    t->align = 1;
    t->size = 0;
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->record.tag = tag_intern(tag);
    t->record.fields = NULL;
    t->record.complete = false;
    t->record.qual_variants = NULL;
    strmap_set(tag_table, t->record.tag, t);
    return t;
}

/* An anonymous record definition (`typedef struct { ... } Name;`,
   `struct { ... } v;`): a *fresh* tagless type, never interened by tag, never
   in tag_table. C11 §6.7.2.1p7: each such definition is a distinct type. The
   caller parses the member list and completes it with type_record_complete. */
Type *type_record_anon(TypeKind kind)
{
    ASSERT(kind == TYPE_STRUCT || kind == TYPE_UNION);
    type_init_tags();

    Type *t = arena_alloc(tag_arena, sizeof(Type), _Alignof(Type));
    t->kind = kind;
    t->width = 0;
    t->align = 1;
    t->size = 0;
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->record.tag = NULL;
    t->record.fields = NULL;
    t->record.complete = false;
    t->record.qual_variants = NULL;
    return t;
}

static u32 align_up(u32 n, u32 align)
{
    return (n + align - 1) / align * align;
}

void type_record_complete(Type *t, Vec *fields)
{
    ASSERT(type_is_record(t));
    t = type_base(t);
    t->record.fields = fields;

    u32 max_align = 1;
    size_t n = vec_size(fields);
    for (size_t i = 0; i < n; i++)
    {
        RecordField *f = (RecordField *) vec_get(fields, i);
        if (f->type->align > max_align)
        {
            max_align = f->type->align;
        }
    }

    if (t->kind == TYPE_STRUCT)
    {
        u32 offset = 0;
        for (size_t i = 0; i < n; i++)
        {
            RecordField *f = (RecordField *) vec_get(fields, i);
            offset = align_up(offset, f->type->align);
            f->offset = offset;
            offset += (u32) f->type->size;
        }
        t->align = max_align;
        t->size = align_up(offset, max_align);
    }
    else
    {
        u32 max_size = 0;
        for (size_t i = 0; i < n; i++)
        {
            RecordField *f = (RecordField *) vec_get(fields, i);
            f->offset = 0;
            if (f->type->size > max_size)
            {
                max_size = (u32) f->type->size;
            }
        }
        t->align = max_align;
        t->size = align_up(max_size, max_align);
    }

    t->record.complete = true;

    /* Propagate layout/completeness to any earlier-built qualified variants
       (e.g. `const struct S *p;` before `struct S { ... };`). */
    if (t->record.qual_variants)
    {
        size_t nv = vec_size(t->record.qual_variants);
        for (size_t i = 0; i < nv; i++)
        {
            Type *v = (Type *) vec_get(t->record.qual_variants, i);
            v->width = t->width;
            v->align = t->align;
            v->size = t->size;
            v->record.fields = t->record.fields;
            v->record.complete = true;
        }
    }
}

Type *type_enum(const char *tag)
{
    type_init_tags();

    Type *existing = strmap_get(tag_table, tag);
    if (existing)
    {
        return existing;
    }

    Type *t = arena_alloc(tag_arena, sizeof(Type), _Alignof(Type));
    t->kind = TYPE_ENUM;
    t->width = type_int()->width;
    t->align = type_int()->align;
    t->size = type_int()->size;
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->enumm.tag = tag_intern(tag);
    t->enumm.complete = false;
    strmap_set(tag_table, t->enumm.tag, t);
    return t;
}

Type *type_enum_anon(void)
{
    type_init_tags();

    /* Anonymous enum definition (`enum { A, B } v;`, `typedef enum { ... }
       E;`): a fresh type, never interned by tag (C11: distinct type per
       definition). */
    Type *t = arena_alloc(tag_arena, sizeof(Type), _Alignof(Type));
    t->kind = TYPE_ENUM;
    t->width = type_int()->width;
    t->align = type_int()->align;
    t->size = type_int()->size;
    t->qualifiers = 0;
    t->unqual_base = NULL;
    t->enumm.tag = NULL;
    t->enumm.complete = true;
    return t;
}

Type *type_record_lookup(const char *tag)
{
    if (!tag_table)
    {
        return NULL;
    }
    return strmap_get(tag_table, tag);
}

static RecordField *find_record_field(Type *t, const char *name)
{
    ASSERT(type_is_record(t));
    t = type_base(t);
    if (!t->record.fields)
    {
        return NULL;
    }
    size_t n = vec_size(t->record.fields);
    for (size_t i = 0; i < n; i++)
    {
        RecordField *f = (RecordField *) vec_get(t->record.fields, i);
        if (strcmp(f->name, name) == 0)
        {
            return f;
        }
    }
    return NULL;
}

Type *type_record_field(Type *t, const char *name)
{
    RecordField *f = find_record_field(t, name);
    return f ? f->type : NULL;
}

u32 type_record_field_offset(Type *t, const char *name)
{
    RecordField *f = find_record_field(t, name);
    return f ? f->offset : 0;
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
    return type_base(t)->size;
}

u64 type_alignof(Type *t)
{
    ASSERT(t->kind != TYPE_VOID);
    return type_base(t)->align;
}

i64 type_reduce_int(Type *target, i64 value)
{
    ASSERT(type_is_integer(target));
    u8 width = target->width;
    if (width >= 64)
    {
        return value;
    }
    u64 mask = ((u64) 1 << width) - 1;
    i64 m = (i64) ((u64) value & mask);
    if (type_is_signed(target) && (m & ((i64) 1 << (width - 1))))
    {
        m |= ~(i64) mask;
    }
    return m;
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
