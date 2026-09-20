#include "abi.h"
#include "util/vec.h"

/* Pure SysV AMD64 eightbyte classification; no emitter reach. */

static bool class_is_x87(ArgClass c)
{
    return c == AC_X87 || c == AC_X87UP || c == AC_COMPLEX_X87;
}

/* psABI §3.2.3 rule 4: pairwise merge of the classes a lane has seen. */
static ArgClass merge_classes(ArgClass a, ArgClass b)
{
    if (a == b)
    {
        return a;
    }
    if (a == AC_NO_CLASS)
    {
        return b;
    }
    if (b == AC_NO_CLASS)
    {
        return a;
    }
    if (a == AC_MEMORY || b == AC_MEMORY)
    {
        return AC_MEMORY;
    }
    if (a == AC_INTEGER || b == AC_INTEGER)
    {
        return AC_INTEGER;
    }
    if (class_is_x87(a) || class_is_x87(b))
    {
        return AC_MEMORY;
    }
    return AC_SSE;
}

static ArgClass scalar_class(Type *t)
{
    switch (t->kind)
    {
        case TYPE_INT:
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_SHORT:
        case TYPE_LONG:
        case TYPE_LLONG:
        case TYPE_UCHAR:
        case TYPE_USHORT:
        case TYPE_UINT:
        case TYPE_ULONG:
        case TYPE_ULLONG:
        case TYPE_PTR:
        case TYPE_ENUM:
            return AC_INTEGER;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
            return AC_SSE;
        case TYPE_LONG_DOUBLE:
            return AC_X87;
        default:
            return AC_NO_CLASS;
    }
}

static void set_lane(ArgClass *lanes, u32 lane, ArgClass c)
{
    if (lane < 8)
    {
        lanes[lane] = merge_classes(lanes[lane], c);
    }
}

/* Merge `t`'s class into every eightbyte `t` spans at offset `off`, following
   the psABI's "fields are considered in pairs, merged per eightbyte". */
static void classify_at(Type *t, u32 off, ArgClass *lanes)
{
    switch (t->kind)
    {
        case TYPE_ARRAY:
            for (u64 i = 0; i < t->arr.length; i++)
            {
                classify_at(t->arr.elem, off + (u32) (i * t->arr.elem->size), lanes);
            }
            return;
        case TYPE_STRUCT:
        {
            size_t n = vec_size(t->record.fields);
            for (size_t i = 0; i < n; i++)
            {
                RecordField *f = (RecordField *) vec_get(t->record.fields, i);
                if (f->bit_width >= 0)
                {
                    continue; /* bit-fields: the layout is opaque to ficc */
                }
                classify_at(type_unqual(f->type), off + f->offset, lanes);
            }
            return;
        }
        case TYPE_UNION:
        {
            size_t n = vec_size(t->record.fields);
            for (size_t i = 0; i < n; i++)
            {
                RecordField *f = (RecordField *) vec_get(t->record.fields, i);
                if (f->bit_width >= 0)
                {
                    continue;
                }
                classify_at(type_unqual(f->type), off, lanes);
            }
            return;
        }
        default:
            break;
    }

    u32 size = t->size != 0 ? t->size : 1;
    if (t->kind == TYPE_LONG_DOUBLE)
    {
        set_lane(lanes, off / 8, AC_X87);
        set_lane(lanes, off / 8 + 1, AC_X87UP);
        return;
    }
    ArgClass c = scalar_class(t);
    u32 first = off / 8;
    u32 last = (off + size - 1) / 8;
    for (u32 lane = first; lane <= last && lane < 8; lane++)
    {
        set_lane(lanes, lane, c);
    }
}

SysVEightByte sysv_eightbyte_split(Type *t)
{
    SysVEightByte e = {0};
    ArgClass lanes[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    Type *base = type_unqual(t);
    if (base->kind == TYPE_VOID || base->kind == TYPE_FUNC)
    {
        return e;
    }
    u32 size = (u32) type_sizeof(base);
    if (size == 0)
    {
        return e; /* empty structures and unions classify NO_CLASS */
    }

    /* psABI §3.2.3 rule 1: objects larger than eight eightbytes ride memory. */
    if (size > 64)
    {
        e.classes[0] = AC_MEMORY;
        e.neightbytes = 1;
        return e;
    }

    classify_at(base, 0, lanes);
    u8 nlanes = (u8) ((size + 7) / 8);

    /* Post-merge cleanup (psABI §3.2.3 rule 5). */
    bool memory = false;
    for (u8 i = 0; i < nlanes && i < 8; i++)
    {
        if (lanes[i] == AC_MEMORY)
        {
            memory = true;
        }
        /* 5b: X87UP must follow X87, or the whole object rides memory. */
        if (lanes[i] == AC_X87UP && (i == 0 || lanes[i - 1] != AC_X87))
        {
            memory = true;
        }
    }
    /* 5c: more than two eightbytes pass only as a vector (first SSE, then
       SSEUP); ficc has no vector types, so this collapses to memory. */
    if (nlanes > 2)
    {
        bool vector = lanes[0] == AC_SSE;
        for (u8 i = 1; i < nlanes && i < 8; i++)
        {
            if (lanes[i] != AC_SSEUP)
            {
                vector = false;
            }
        }
        if (!vector)
        {
            memory = true;
        }
    }
    /* 5d: an SSEUP lane not preceded by SSE/SSEUP downgrades to SSE. */
    for (u8 i = 1; i < nlanes && i < 8; i++)
    {
        if (lanes[i] == AC_SSEUP && lanes[i - 1] != AC_SSE && lanes[i - 1] != AC_SSEUP)
        {
            lanes[i] = AC_SSE;
        }
    }

    if (memory)
    {
        e.classes[0] = AC_MEMORY;
        e.neightbytes = 1;
        return e;
    }

    e.neightbytes = nlanes < 4 ? nlanes : 4;
    for (u8 i = 0; i < e.neightbytes; i++)
    {
        e.classes[i] = lanes[i];
    }
    return e;
}

bool sysv_eightbyte_register_passed(const SysVEightByte *e)
{
    for (u8 i = 0; i < e->neightbytes; i++)
    {
        ArgClass c = e->classes[i];
        if (c == AC_MEMORY || c == AC_X87 || c == AC_X87UP || c == AC_COMPLEX_X87)
        {
            return false;
        }
    }
    return true;
}

bool sysv_eightbyte_return_sret(const SysVEightByte *e)
{
    return e->neightbytes == 1 && e->classes[0] == AC_MEMORY;
}

u8 sysv_eightbyte_gp_count(const SysVEightByte *e)
{
    u8 n = 0;
    for (u8 i = 0; i < e->neightbytes; i++)
    {
        if (e->classes[i] == AC_INTEGER)
        {
            n++;
        }
    }
    return n;
}

u8 sysv_eightbyte_xmm_count(const SysVEightByte *e)
{
    u8 n = 0;
    for (u8 i = 0; i < e->neightbytes; i++)
    {
        if (e->classes[i] == AC_SSE)
        {
            n++;
        }
    }
    return n;
}