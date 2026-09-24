#ifndef FICC_X86_LINK_H
#define FICC_X86_LINK_H

#include "elfdefs.h"
#include "util/bytebuf.h"
#include "util/types.h"

/* Outcome of applying one relocation to a field of known width. */
typedef enum
{
    RELOC_OK,
    RELOC_OVERFLOW,
    RELOC_UNSUPPORTED,
} RelocResult;

/* The arch-specific half of the linker: relocation kinds and application, the
   freestanding entry stub, and PLT stub bytes. `link.c` reaches x86 only
   through this descriptor, exactly as the backend reaches the target through
   `TargetDesc`; it never sees the linker context. */
typedef struct LinkArch LinkArch;
struct LinkArch
{
    u16 machine; /* EM_X86_64 */
    u32 reloc_none, reloc_64, reloc_pc32, reloc_32, reloc_32s, reloc_plt32, reloc_pc64;

    /* Apply `type` at `field` (S = symbol value, A = addend, P = field address). */
    RelocResult (*apply_reloc)(u32 type, u8 *field, u64 s, i64 addend, u64 place);

    /* Freestanding `_start`: call main, then exit(status) via syscall 60.
       Returns the offset within the stub of the rel32 field targeting main. */
    u64 (*emit_start)(ByteBuf *out);
};

const LinkArch *x86_link_arch(void);

#endif
