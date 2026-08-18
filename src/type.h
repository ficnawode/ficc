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

/* X-macro for type kinds. Append only. */
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
    const char *name; /* field name, compared by content (not interned) */
    Type *type;
    u32 offset; /* byte offset in the record (always 0 for unions) */
};

struct Type
{
    TypeKind kind;
    u8 width; /* in bits */
    u8 align; /* in bytes */
    u32 size; /* in bytes */
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
        } arr;
        struct
        {
            const char *tag; /* NULL only for anonymous (out of scope) */
            Vec *fields;     /* Vec<RecordField*> */
            bool complete;
        } record; /* TYPE_STRUCT / TYPE_UNION */
        struct
        {
            const char *tag; /* identity + diagnostics */
        } enumm;             /* TYPE_ENUM: underlying int, no members */
    };
};

Type *type_void(void);
Type *type_int(void);
Type *type_cbool(void); /* C11 _Bool: 1 byte, unsigned semantics */
Type *type_char(void);
Type *type_short(void);
Type *type_long(void);
Type *type_llong(void);
Type *type_uchar(void);
Type *type_ushort(void);
Type *type_uint(void);
Type *type_ulong(void);
Type *type_ullong(void);

bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_integer(Type *t);
bool type_is_ptr(Type *t);
bool type_is_array(Type *t);
bool type_is_record(Type *t);
bool type_is_struct(Type *t);
bool type_is_union(Type *t);
bool type_is_enum(Type *t);
bool type_is_complete(Type *t); /* records only */

Type *type_ptr(Type *pointee);
Type *type_array(Type *elem, u64 length);
Type *type_record(TypeKind kind, const char *tag);
void type_record_complete(Type *t, Vec *fields);
Type *type_enum(const char *tag);
Type *type_record_lookup(const char *tag);
Type *type_record_field(Type *t, const char *name);
u32 type_record_field_offset(Type *t, const char *name);

/* Reset the tag table for a new compilation unit. Record types remain
   immortal (they may be referenced by interned pointer/array types), but the
   tag-name mapping is cleared so a later compilation may reuse tag names. */
void type_reset(void);
Type *type_deref(Type *t);
Type *type_array_elem(Type *t);
u64 type_array_len(Type *t);
Type *type_decay(Type *t);
u64 type_sizeof(Type *t);

/* C11 §6.3.1.1 integer promotion: promote types narrower than int to int. */
Type *type_promote(Type *t);

/* C11 §6.3.1.8 usual arithmetic conversions: common type for binary operations. */
Type *type_common(Type *a, Type *b);

/* C11 §6.4.4.1 integer constant typing. Returns the smallest type that can
   represent `value` given the suffix constraints, per LP64 data model. */
Type *type_int_literal(i64 value, bool is_hex, bool is_unsigned, IntSuffix length);

/* Integer rank for type comparison: higher rank = wider type.
   Returns -1 for non-integer types. */
int type_rank(Type *t);

const char *type_kind_name(TypeKind kind);

#endif
