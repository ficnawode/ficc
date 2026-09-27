#ifndef FICC_X86_LINK_H
#define FICC_X86_LINK_H

#include "elfdefs.h"
#include "util/bytebuf.h"
#include "util/types.h"

typedef enum
{
    RELOC_OK,
    RELOC_OVERFLOW,
    RELOC_UNSUPPORTED,
} RelocResult;

typedef struct LinkArch LinkArch;
struct LinkArch
{
    u16 machine;
    u32 reloc_none, reloc_64, reloc_pc32, reloc_32, reloc_32s, reloc_plt32, reloc_pc64;

    RelocResult (*apply_reloc)(u32 type, u8 *field, u64 s, i64 addend, u64 place);

    u64 (*emit_start)(ByteBuf *out);
};

const LinkArch *x86_link_arch(void);

#endif
