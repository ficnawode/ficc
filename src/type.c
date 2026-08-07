#include "type.h"
#include "util/assert.h"

/* Integer type singletons. LP64 data model:
   char=8, short=16, int=32, long=64, long long=64. */
static const Type the_void   = {TYPE_VOID,    0, 1, 0};
static const Type the_bool   = {TYPE_BOOL,    8, 1, 1};
static const Type the_char   = {TYPE_CHAR,    8, 1, 1};
static const Type the_short  = {TYPE_SHORT,  16, 2, 2};
static const Type the_int    = {TYPE_INT,    32, 4, 4};
static const Type the_long   = {TYPE_LONG,   64, 8, 8};
static const Type the_llong  = {TYPE_LLONG,  64, 8, 8};
static const Type the_uchar  = {TYPE_UCHAR,   8, 1, 1};
static const Type the_ushort = {TYPE_USHORT, 16, 2, 2};
static const Type the_uint   = {TYPE_UINT,   32, 4, 4};
static const Type the_ulong  = {TYPE_ULONG,  64, 8, 8};
static const Type the_ullong = {TYPE_ULLONG, 64, 8, 8};

Type *type_void(void)   { return (Type *)&the_void; }
Type *type_cbool(void)  { return (Type *)&the_bool; }  /* C11 _Bool */
Type *type_char(void)   { return (Type *)&the_char; }
Type *type_short(void)  { return (Type *)&the_short; }
Type *type_int(void)    { return (Type *)&the_int; }
Type *type_long(void)   { return (Type *)&the_long; }
Type *type_llong(void)  { return (Type *)&the_llong; }
Type *type_uchar(void)  { return (Type *)&the_uchar; }
Type *type_ushort(void) { return (Type *)&the_ushort; }
Type *type_uint(void)   { return (Type *)&the_uint; }
Type *type_ulong(void)  { return (Type *)&the_ulong; }
Type *type_ullong(void) { return (Type *)&the_ullong; }

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
        case TYPE_BOOL:   return 0;
        case TYPE_CHAR:
        case TYPE_UCHAR:  return 1;
        case TYPE_SHORT:
        case TYPE_USHORT: return 2;
        case TYPE_INT:
        case TYPE_UINT:   return 3;
        case TYPE_LONG:
        case TYPE_ULONG:  return 4;
        case TYPE_LLONG:
        case TYPE_ULLONG: return 5;
        default:          return -1;
    }
}

Type *type_promote(Type *t)
{
    /* C11 §6.3.1.1: if an int can represent all values of the original type,
       the value is converted to int; otherwise unsigned int. */
    if (!type_is_integer(t))
    {
        return (Type *)t;
    }
    if (type_rank(t) < type_rank(type_int()))
    {
        return type_int();
    }
    return (Type *)t;
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
