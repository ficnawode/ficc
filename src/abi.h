#ifndef FICC_ABI_H
#define FICC_ABI_H

#include "type.h"
#include "util/types.h"

/* AMD64 psABI §3.2.3 eightbyte classes (NO_CLASS zero: the lane initializer). */
typedef enum
{
    AC_NO_CLASS = 0, /* padding / empty structures and unions */
    AC_INTEGER,      /* fits a general-purpose register */
    AC_SSE,          /* fits a vector register */
    AC_SSEUP,        /* upper bytes of the last-used vector register */
    AC_X87,          /* long double mantissa */
    AC_X87UP,        /* long double exponent and padding */
    AC_COMPLEX_X87,  /* complex long double */
    AC_MEMORY,       /* passed/returned via the stack (sret for returns) */
} ArgClass;

/* An argument's classification: one class per eightbyte, in memory layout
   order.  Register-passed aggregates use at most two lanes; any other outcome
   collapses to a single AC_MEMORY lane (post-cleanup rules 5a/5c). */
typedef struct
{
    ArgClass classes[4];
    u8 neightbytes;
} SysVEightByte;

/* AMD64 psABI §3.2.3 aggregate/union/array classification (rule 1 folded in:
   larger-than-64-byte objects return AC_MEMORY).  Size is rounded up to
   eightbytes; each field's class merges into every eightbyte it spans. */
SysVEightByte sysv_eightbyte_split(Type *t);

/* The argument rides registers iff every lane is INTEGER/SSE/SSEUP; X87*,
   COMPLEX_X87 and MEMORY lanes are passed on the stack. */
bool sysv_eightbyte_register_passed(const SysVEightByte *e);

/* Aggregate RETURN class: MEMORY returns ride a hidden sret pointer, everything
   else comes back through the register pair (RAX:RDX, or XMM0 lanes). */
bool sysv_eightbyte_return_sret(const SysVEightByte *e);

/* Register-consumption counts for handing lanes to the ABI registers. */
u8 sysv_eightbyte_gp_count(const SysVEightByte *e);
u8 sysv_eightbyte_xmm_count(const SysVEightByte *e);

#endif