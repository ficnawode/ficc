#ifndef FICC_TYPE_H
#define FICC_TYPE_H

#include "util/arena.h"
#include "util/types.h"

typedef enum
{
    SUFFIX_NONE,
    SUFFIX_L,
    SUFFIX_LL,
} IntSuffix;

/* C11 §6.7.3 */
typedef enum
{
    Q_NONE = 0,
    Q_CONST = 1 << 0,
    Q_VOLATILE = 1 << 1,
    Q_RESTRICT = 1 << 2,
} Qualifier;

#define TYPE_KINDS(X)                                                                              \
    X(TYPE_VOID)                                                                                   \
    X(TYPE_INT)                                                                                    \
    X(TYPE_BOOL)                                                                                   \
    X(TYPE_CHAR)                                                                                   \
    X(TYPE_SHORT)                                                                                  \
    X(TYPE_LONG)                                                                                   \
    X(TYPE_LLONG)                                                                                  \
    X(TYPE_UCHAR)                                                                                  \
    X(TYPE_USHORT)                                                                                 \
    X(TYPE_UINT)                                                                                   \
    X(TYPE_ULONG)                                                                                  \
    X(TYPE_ULLONG)                                                                                 \
    X(TYPE_FLOAT)                                                                                  \
    X(TYPE_DOUBLE)                                                                                 \
    X(TYPE_LONG_DOUBLE)                                                                            \
    X(TYPE_PTR)                                                                                    \
    X(TYPE_ARRAY)                                                                                  \
    X(TYPE_STRUCT)                                                                                 \
    X(TYPE_UNION)                                                                                  \
    X(TYPE_FUNC)                                                                                   \
    X(TYPE_ENUM)

typedef enum
{
#define ENUM_ENTRY(K) K,
    TYPE_KINDS(ENUM_ENTRY)
#undef ENUM_ENTRY
} TypeKind;

typedef struct Type Type;
typedef struct Vec Vec;
typedef struct RecordField RecordField;

struct RecordField
{
    const char *name;
    Type *type;
    u32 offset;
    i32 bit_offset;
    i32 bit_width;
    u32 align_override;
};

struct Type
{
    TypeKind kind;
    u8 width;
    u8 align;
    u32 size;
    u8 qualifiers;
    Type *unqual_base;
    union
    {
        struct
        {
            Type *pointee;
        } ptr;
        struct
        {
            Type *elem;
            u64 length;
            struct ASTNode *bound_expr; /* C11 §6.7.6.2 */
        } arr;
        struct
        {
            const char *tag;
            Vec *fields; /* Vec<RecordField*> */
            bool complete;
            bool packed;
            u32 align_override;
            Vec *qual_variants; /* Vec<Type*> */
        } record;
        struct
        {
            const char *tag;
            bool complete;
        } enumm;
        struct
        {
            Type *ret;   /* C11 §6.7.6.3p15 */
            Vec *params; /* Vec<Type*> */
            bool is_variadic;
        } func;
    };
};

Type *type_void(void);
Type *type_int(void);
Type *type_cbool(void);
Type *type_char(void);
Type *type_short(void);
Type *type_long(void);
Type *type_llong(void);
Type *type_uchar(void);
Type *type_ushort(void);
Type *type_uint(void);
Type *type_ulong(void);
Type *type_ullong(void);

Type *type_float(void);
Type *type_double(void);

Type *type_long_double(void);

bool type_is_signed_int(Type *t);
bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_integer(Type *t);
bool type_is_float(Type *t);
bool type_is_fp(Type *t);
bool type_is_ptr(Type *t);
bool type_is_array(Type *t);
bool type_is_record(Type *t);
bool type_is_struct(Type *t);
bool type_is_union(Type *t);
bool type_is_enum(Type *t);
bool type_is_complete(Type *t);
bool type_is_const(Type *t);
bool type_is_volatile(Type *t);

Type *type_ptr(Type *pointee);
Type *type_array(Type *elem, u64 length);

Type *type_array_pending(Type *elem, struct ASTNode *bound_expr);
Type *type_array_resolve(Type *pending, u64 length);
bool type_array_is_pending(Type *t);

Type *type_func(Type *ret, Vec *params, bool is_variadic);
bool type_is_variadic(Type *t);
Type *type_record(TypeKind kind, const char *tag);
Type *type_record_anon(TypeKind kind);
void type_record_complete(Type *t, Vec *fields);
void type_record_relayout(Type *t);

Type *type_va_list(void);
Type *type_enum(const char *tag);
Type *type_enum_anon(void);
Type *type_record_lookup(const char *tag);
void type_tag_scope_push(void);
void type_tag_scope_pop(void);
bool type_record_has_fam(Type *t);
Type *type_record_field(Type *t, const char *name);
u32 type_record_field_offset(Type *t, const char *name);

bool type_record_field_bit(Type *t, const char *name, u32 *bit_offset, u32 *bit_width);

void type_reset(void);
Type *type_deref(Type *t);
Type *type_array_elem(Type *t);
u64 type_array_len(Type *t);
Type *type_decay(Type *t);
bool type_is_function(Type *t);
u64 type_sizeof(Type *t);
u64 type_alignof(Type *t);

/* C11 §6.7.3 */
Type *type_const(Type *t);
Type *type_volatile(Type *t);
Type *type_restrict(Type *t);
Type *type_qualify(Type *t, u8 qbits);
Type *type_unqual(Type *t);
Type *type_rvalue(Type *t);

/* C11 §6.3.1.1 */
Type *type_promote(Type *t);

/* C11 §6.3.1.8 */
Type *type_common(Type *a, Type *b);

/* C11 §6.2.7, §6.5.15p2 */
bool type_compatible(Type *a, Type *b);

/* C11 §6.4.4.1 */
Type *type_int_literal(i64 value, bool is_hex, bool is_unsigned, IntSuffix length);

int type_rank(Type *t);

/* C11 §6.3.1.3 */
i64 type_reduce_int(Type *target, i64 value);

const char *type_kind_name(TypeKind kind);

#endif
