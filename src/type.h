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

/* Type qualifiers (C11 §6.7.3). `const` today; volatile/restrict land later
   in the same bitmask (append-only — do not reorder or repurpose bits). */
typedef enum
{
    Q_NONE = 0,
    Q_CONST = 1 << 0,
} Qualifier;

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
    u32 offset;     /* byte offset of the member (bit-field: of its storage unit) */
    i32 bit_offset; /* bit-field: bit position within the storage unit, else -1 */
    i32 bit_width;  /* bit-field width, else -1 */
};

struct Type
{
    TypeKind kind;
    u8 width;          /* in bits */
    u8 align;          /* in bytes */
    u32 size;          /* in bytes */
    u8 qualifiers;     /* V8 qualifier bitmask (Q_CONST) */
    Type *unqual_base; /* the unqualified type this qualified variant wraps
                          (NULL for unqualified types; arrays store no
                          wrapper — const lives in the element type) */
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
            Vec *qual_variants; /* Vec<Type*>: const variants of this record,
                                   kept in sync by type_record_complete */
        } record;               /* TYPE_STRUCT / TYPE_UNION */
        struct
        {
            const char *tag; /* identity + diagnostics */
            bool complete;   /* set by the enum definition */
        } enumm;             /* TYPE_ENUM: underlying int, no members */
        struct
        {
            /* top-level qualifiers stripped (C11 §6.7.6.3p15) */
            Type *ret;
            /* Vec<Type*>, already type_unqual'd; empty for (void) */
            Vec *params;
            bool is_variadic;
        } func; /* TYPE_FUNC */
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

bool type_is_signed_int(Type *t);
bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_integer(Type *t);
bool type_is_ptr(Type *t);
bool type_is_array(Type *t);
bool type_is_record(Type *t);
bool type_is_struct(Type *t);
bool type_is_union(Type *t);
bool type_is_enum(Type *t);
bool type_is_complete(Type *t); /* records + arrays (len-0 = `[]` unspecified, D12.7) */
bool type_is_const(Type *t);

Type *type_ptr(Type *pointee);
Type *type_array(Type *elem, u64 length);
Type *type_func(Type *ret, Vec *params, bool is_variadic);
bool type_is_variadic(Type *t);
Type *type_record(TypeKind kind, const char *tag);
Type *type_record_anon(TypeKind kind);
void type_record_complete(Type *t, Vec *fields);

/* The builtin `va_list` (D15.2): glibc's x86-64 shape, a 24-byte struct
   `{u32 gp_offset; u32 fp_offset; void *overflow_arg_area; void
   *reg_save_area;}` used as an array of 1 so it decays to a pointer on use.
   Interned singleton; reconstructed per compilation unit by type_reset. */
Type *type_va_list(void);
Type *type_enum(const char *tag);
Type *type_enum_anon(void);
Type *type_record_lookup(const char *tag);
Type *type_record_field(Type *t, const char *name);
u32 type_record_field_offset(Type *t, const char *name);

/* Bit-field info for `name` (recursing through anonymous members): true when
   the member is a bit-field, filling its position within the storage unit and
   width; false fills nothing. */
bool type_record_field_bit(Type *t, const char *name, u32 *bit_offset, u32 *bit_width);

/* Reset the tag table for a new compilation unit. Record types remain
   immortal (they may be referenced by interned pointer/array types), but the
   tag-name mapping is cleared so a later compilation may reuse tag names. */
void type_reset(void);
Type *type_deref(Type *t);
Type *type_array_elem(Type *t);
u64 type_array_len(Type *t);
Type *type_decay(Type *t);
bool type_is_function(Type *t);
u64 type_sizeof(Type *t);
u64 type_alignof(Type *t);

/* Qualifier composition (C11 §6.7.3). Qualifying an array qualifies its
   element type (`const int a[3]` is an array of const int), so decay yields
   `const int*` and `a[i]` lvalues are const. Qualifying is idempotent.
   `type_unqual` removes every qualifier (rvalues are always unqualified). */
Type *type_const(Type *t);
Type *type_unqual(Type *t);
Type *type_rvalue(Type *t); /* alias for type_unqual: strip qualifiers on read */

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

/* C11 §6.3.1.3: convert a value into a target integer type's range. Narrower
   targets wrap modulo 2^width; the result is masked to the width and
   sign-extended if the target is signed (the implementation-defined wrap for
   out-of-range signed targets, matching design.md §8). Used by constant
   folding of casts and switch case conversion; runtime casts lower to
   TRUNC/ZEXT/SEXT in the IR builder instead. */
i64 type_reduce_int(Type *target, i64 value);

const char *type_kind_name(TypeKind kind);

#endif
