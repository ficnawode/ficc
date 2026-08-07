#ifndef FICC_TYPE_H
#define FICC_TYPE_H

#include "util/arena.h"
#include "util/types.h"

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

struct Type
{
    TypeKind kind;
    u8 width; /* in bits */
    u8 align; /* in bytes */
    u32 size; /* in bytes */
    /* kind-specific payload goes here */
};

Type *type_void(void);
Type *type_int(void);
Type *type_cbool(void);  /* C11 _Bool: 1 byte, unsigned semantics */
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

/* C11 §6.3.1.1 integer promotion: promote types narrower than int to int. */
Type *type_promote(Type *t);

/* C11 §6.3.1.8 usual arithmetic conversions: common type for binary operations. */
Type *type_common(Type *a, Type *b);

/* Integer rank for type comparison: higher rank = wider type.
   Returns 0 for non-integer types. */
int type_rank(Type *t);

const char *type_kind_name(TypeKind kind);

#endif
