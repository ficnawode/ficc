#ifndef FICC_ABI_H
#define FICC_ABI_H

#include "type.h"
#include "util/types.h"

/* AMD64 psABI §3.2.3 eightbyte classes */
typedef enum
{
    AC_NO_CLASS = 0,
    AC_INTEGER,
    AC_SSE,
    AC_SSEUP,
    AC_X87,
    AC_X87UP,
    AC_COMPLEX_X87,
    AC_MEMORY,
} ArgClass;

/* AMD64 psABI §3.2.3 cleanup rules 5a/5c */
typedef struct
{
    ArgClass classes[4];
    u8 neightbytes;
} SysVEightByte;

/* AMD64 psABI §3.2.3 aggregate/union/array classification */
SysVEightByte sysv_eightbyte_split(Type *t);

bool sysv_eightbyte_register_passed(const SysVEightByte *e);

bool sysv_eightbyte_return_sret(const SysVEightByte *e);

u8 sysv_eightbyte_gp_count(const SysVEightByte *e);
u8 sysv_eightbyte_xmm_count(const SysVEightByte *e);

#endif