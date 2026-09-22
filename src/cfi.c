#include "cfi.h"
#include "util/assert.h"

/* .eh_frame emission (DWARF4 §6.4.2, the two-address frame table). */

enum
{
    DW_CFA_nop = 0x00,
    DW_CFA_advance_loc1 = 0x02, /* GNU .eh_frame encoding: 1-byte delta */
    DW_CFA_advance_loc2 = 0x03, /* GNU .eh_frame encoding: 2-byte delta */
    DW_CFA_advance_loc4 = 0x04, /* GNU .eh_frame encoding: 4-byte delta */
    DW_CFA_advance_loc = 0x40,  /* + delta, delta < 64 */
    DW_CFA_offset = 0x80,       /* + reg, ULEB factor-adjusted slot */
    DW_CFA_def_cfa = 0x0c,
    DW_CFA_def_cfa_register = 0x0d,
    DW_CFA_def_cfa_offset = 0x0e,
};

/* SysV AMD64 DWARF register numbers for the CFI rules. */
enum
{
    CFA_REG_RBP = 6,
    CFA_REG_RSP = 7,
    CFA_REG_RIP = 16,
};

/* CIE data alignment factor -8: every slot below the CFA is a multiple of 8. */
#define CFA_DATA_ALIGN (-8)

/* FDE pc fields are absptr: zeros + R_X86_64_64 vs the .text section symbol. */
#define EH_PE_ABSPTR 0x00

#define EH_ENTRY_ALIGN 8

/* unsigned LEB128 (DWARF4 §7.6): 7 bits per byte, high bit = more follows. */
static void uleb128(ByteBuf *b, u64 val)
{
    for (;;)
    {
        u8 byte = (u8) (val & 0x7f);
        val >>= 7;
        if (val)
        {
            byte |= 0x80;
        }
        bytebuf_append(b, byte);
        if (!val)
        {
            break;
        }
    }
}

/* signed LEB128: little-endian sign extension through the padding bits. */
static void sleb128(ByteBuf *b, i64 val)
{
    bool more = true;
    while (more)
    {
        u8 byte = (u8) (val & 0x7f);
        val >>= 7;
        bool sign_bit = (byte & 0x40) != 0;
        more = !((val == 0 && !sign_bit) || (val == -1 && sign_bit));
        if (more)
        {
            byte |= 0x80;
        }
        bytebuf_append(b, byte);
    }
}

/* Deltas ride 0x02/0x03/0x04 with 1/2/4-byte operands (libgcc's op-code map). */
static void cfa_advance(ByteBuf *b, u32 delta)
{
    if (delta < 64)
    {
        bytebuf_append(b, (u8) (DW_CFA_advance_loc | delta));
        return;
    }
    if (delta <= 0xFF)
    {
        bytebuf_append(b, DW_CFA_advance_loc1);
        bytebuf_append(b, (u8) delta);
        return;
    }
    if (delta <= 0xFFFF)
    {
        bytebuf_append(b, DW_CFA_advance_loc2);
        bytebuf_append_u16(b, (u16) delta);
        return;
    }
    bytebuf_append(b, DW_CFA_advance_loc4);
    bytebuf_append_u32(b, delta);
}

static void cfa_def_cfa(ByteBuf *b, u32 reg, u32 offset)
{
    bytebuf_append(b, DW_CFA_def_cfa);
    uleb128(b, reg);
    uleb128(b, offset);
}

static void cfa_def_cfa_register(ByteBuf *b, u32 reg)
{
    bytebuf_append(b, DW_CFA_def_cfa_register);
    uleb128(b, reg);
}

static void cfa_def_cfa_offset(ByteBuf *b, u32 offset)
{
    bytebuf_append(b, DW_CFA_def_cfa_offset);
    uleb128(b, offset);
}

/* `reg` lives at CFA + factor * slot (factor is the CIE's -8 data align). */
static void cfa_offset(ByteBuf *b, u32 reg, u32 slot)
{
    bytebuf_append(b, (u8) (DW_CFA_offset | reg));
    uleb128(b, slot);
}

/* Pad an entry to 8 bytes with DW_CFA_nop, folded into its length field. */
static void cfi_entry_finish(ByteBuf *b, size_t body_start)
{
    size_t body = bytebuf_len(b) - body_start;
    size_t pad = (EH_ENTRY_ALIGN - ((sizeof(u32) + body) % EH_ENTRY_ALIGN)) % EH_ENTRY_ALIGN;
    for (size_t i = 0; i < pad; i++)
    {
        bytebuf_append(b, DW_CFA_nop);
    }
    body = bytebuf_len(b) - body_start;
    bytebuf_poke_u32(b, body_start - sizeof(u32), (u32) body);
}

/* Emit the CIE at section offset 0 so FDE CIE pointers resolve to it. */
static void cfi_emit_cie(ByteBuf *b)
{
    size_t body_start = bytebuf_len(b) + sizeof(u32);
    bytebuf_append_u32(b, 0); /* length placeholder: patched in cfi_entry_finish */
    bytebuf_append_u32(b, 0); /* CIE id: 0 in .eh_frame */
    bytebuf_append(b, 1);     /* version */
    bytebuf_append_bytes(b, (const u8 *) "zR", 3); /* augmentation + NUL */
    uleb128(b, 1);                                 /* code alignment factor */
    sleb128(b, CFA_DATA_ALIGN);                    /* data alignment factor */
    bytebuf_append(b, CFA_REG_RIP);                /* return address register */
    uleb128(b, 1);                                 /* augmentation data length */
    bytebuf_append(b, EH_PE_ABSPTR);               /* FDE pc encoding: absolute address */

    /* Initial rules: CFA = rsp+8 with the return address at CFA-8. */
    cfa_def_cfa(b, CFA_REG_RSP, 8);
    cfa_offset(b, CFA_REG_RIP, 1);

    cfi_entry_finish(b, body_start);
}

/* FDE rows: CFA rsp+8 -> rsp+16/rbp@CFA-16 -> rbp+16, then back at the ret. */
static void cfi_emit_fde(CfiOutput *out, CodegenFunc *cf)
{
    ByteBuf *b = &out->eh_frame;
    size_t body_start = bytebuf_len(b) + sizeof(u32);
    bytebuf_append_u32(b, 0); /* length placeholder: patched in cfi_entry_finish */

    /* CIE pointer (backward offset) equals the field's own position. */
    bytebuf_append_u32(b, (u32) bytebuf_len(b));
    size_t pc_slot = bytebuf_len(b);                     /* initial_location begins here */
    bytebuf_append_u64(b, 0);                            /* initial_location: RELA resolves this */
    bytebuf_append_u64(b, (u64) bytebuf_len(cf->bytes)); /* address_range: plain size */
    uleb128(b, 0); /* FDE augmentation length: "z" CIEs need the prefix even when empty */

    u32 off_push = cf->frame.off_push;
    u32 off_mov = cf->frame.off_mov;
    u32 off_sub = cf->frame.off_sub;
    ASSERT(off_push <= off_mov && off_mov <= off_sub &&
           "prologue layout steps are ordered for the CFI rows");

    cfa_advance(b, off_push);
    cfa_def_cfa_offset(b, 16);
    cfa_offset(b, CFA_REG_RBP, 2);
    cfa_advance(b, off_mov - off_push);
    cfa_def_cfa_register(b, CFA_REG_RBP);

    /* Callee-saved pushes land one 8-byte slot below the previous, starting at
       slot 3 (rbp itself is slot 2) in the CFA's -8 factored units. */
    u32 pos = off_mov;
    for (u8 i = 0; i < cf->frame.nsaved; i++)
    {
        u8 reg = cf->frame.saved_regs[i];
        u32 push_bytes = reg >= R_R8 ? 2 : 1; /* r8-r15 carry a REX.B prefix byte */
        cfa_advance(b, push_bytes);
        pos += push_bytes;
        cfa_offset(b, x86_dwarf_gpr_number(reg), 3 + i);
    }

    /* At the ret byte the frame is gone: unwind to the caller's CFA. */
    u64 fsize = (u64) bytebuf_len(cf->bytes);
    if (fsize >= 1 && fsize - 1 >= pos)
    {
        cfa_advance(b, (u32) (fsize - 1 - pos));
        cfa_def_cfa(b, CFA_REG_RSP, 8);
    }

    cfi_entry_finish(b, body_start);

    CfiReloc *rel = arena_alloc(out->eh_frame.arena, sizeof(CfiReloc), sizeof(void *));
    rel->offset = (u64) pc_slot;
    rel->addend = (i64) cf->offset;
    vec_push(out->relocs, rel);
}

CfiOutput *cfi_build(CodegenModule *cm, Arena *arena)
{
    CfiOutput *out = arena_alloc(arena, sizeof(CfiOutput), sizeof(void *));
    bytebuf_init(&out->eh_frame, arena);
    out->relocs = vec_new(arena);

    cfi_emit_cie(&out->eh_frame);

    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        cfi_emit_fde(out, cf);
    }
    return out;
}