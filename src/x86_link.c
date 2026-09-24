#include "x86_link.h"

#include <limits.h>
#include <string.h>

static RelocResult apply_x86_reloc(u32 type, u8 *field, u64 s, i64 addend, u64 place)
{
    switch (type)
    {
        case R_X86_64_NONE:
            return RELOC_OK;
        case R_X86_64_64:
        {
            u64 v = s + (u64) addend;
            memcpy(field, &v, 8);
            return RELOC_OK;
        }
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
        {
            i64 v = (i64) s + addend - (i64) place;
            if (v < INT32_MIN || v > INT32_MAX)
            {
                return RELOC_OVERFLOW;
            }
            i32 v32 = (i32) v;
            memcpy(field, &v32, 4);
            return RELOC_OK;
        }
        case R_X86_64_32:
        {
            u64 v = s + (u64) addend;
            if (v > 0xffffffffULL)
            {
                return RELOC_OVERFLOW;
            }
            u32 v32 = (u32) v;
            memcpy(field, &v32, 4);
            return RELOC_OK;
        }
        case R_X86_64_32S:
        {
            i64 v = (i64) s + addend;
            if (v < INT32_MIN || v > INT32_MAX)
            {
                return RELOC_OVERFLOW;
            }
            i32 v32 = (i32) v;
            memcpy(field, &v32, 4);
            return RELOC_OK;
        }
        case R_X86_64_PC64:
        {
            u64 v = s + (u64) addend - place;
            memcpy(field, &v, 8);
            return RELOC_OK;
        }
        default:
            return RELOC_UNSUPPORTED;
    }
}

/* xor ebp,ebp; pop rdi; mov rsi,rsp; and rsp,-16; call main; mov edi,eax;
   mov eax,60; syscall. Returns the offset of the `call main` rel32 field. */
static u64 emit_x86_start(ByteBuf *out)
{
    static const u8 head[] = {0x31, 0xed,                   /* xor ebp,ebp   */
                              0x5f,                         /* pop rdi       */
                              0x48, 0x89, 0xe6,             /* mov rsi,rsp   */
                              0x48, 0x83, 0xe4, 0xf0,       /* and rsp,-16   */
                              0xe8};                        /* call rel32    */
    static const u8 tail[] = {0x89, 0xc7,                   /* mov edi,eax */
                              0xb8, 0x3c, 0x00, 0x00, 0x00, /* mov eax,60  */
                              0x0f, 0x05};                  /* syscall     */
    u64 base = bytebuf_len(out);
    bytebuf_append_bytes(out, head, sizeof(head));
    u64 rel32_off = bytebuf_len(out) - base;
    bytebuf_append_u32(out, 0);
    bytebuf_append_bytes(out, tail, sizeof(tail));
    return rel32_off;
}

static const LinkArch X86_LINK_ARCH = {
    .machine = EM_X86_64,
    .reloc_none = R_X86_64_NONE,
    .reloc_64 = R_X86_64_64,
    .reloc_pc32 = R_X86_64_PC32,
    .reloc_32 = R_X86_64_32,
    .reloc_32s = R_X86_64_32S,
    .reloc_plt32 = R_X86_64_PLT32,
    .reloc_pc64 = R_X86_64_PC64,
    .apply_reloc = apply_x86_reloc,
    .emit_start = emit_x86_start,
};

const LinkArch *x86_link_arch(void)
{
    return &X86_LINK_ARCH;
}
