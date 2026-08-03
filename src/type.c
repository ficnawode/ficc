#include "type.h"

static const Type the_void = {TYPE_VOID, 0, 1, 0};
static const Type the_int = {TYPE_INT, 32, 4, 4};

Type *type_void(void)
{
    return (Type *) &the_void;
}

Type *type_int(void)
{
    return (Type *) &the_int;
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
