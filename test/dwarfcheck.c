#include "dwarfcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* In-process structural readers for ficc's DWARF output (test-only). */

enum
{
    LEB_CONT = 0x80,
    LEB_SIGN = 0x40,
};

enum
{
    ADDR_BYTES = 8, /* ELF64 address slots inside the debug sections */
};

/* DWARF2 line-program standard/extended opcodes. */
enum
{
    LNS_copy = 1,
    LNS_advance_pc,
    LNS_advance_line,
    LNS_set_file,
    LNS_set_column,
    LNS_negate_stmt,
    LNS_set_basic_block,
    LNS_const_add_pc,
    LNS_fixed_advance_pc,
    LNS_set_prologue_end,
    LNS_set_epilogue_begin,
    LNS_set_isa,
};

enum
{
    LNE_end_sequence = 1,
    LNE_set_address = 2,
};

enum
{
    LINE_HDR_FIELDS = 5, /* min_inst, is_stmt, line_base, line_range, opcode_base */
    LINE_LAST_OPCODE = 255,
    LINE_OP_MIN = 0,
    LINE_IS_STMT = 1,
    LINE_BASE = 2,
    LINE_RANGE = 3,
    LINE_OPCODE_BASE = 4,
};

/* DWARF4 attribute forms the abbrev walker knows. */
enum
{
    FORM_addr = 0x01,
    FORM_data1 = 0x0b,
    FORM_data2 = 0x05,
    FORM_data4 = 0x06,
    FORM_udata = 0x0f,
    FORM_string = 0x08,
    FORM_flag = 0x0c,
    FORM_ref4 = 0x13,
    FORM_exprloc = 0x18,
    FORM_sec_offset = 0x17,
};

/* .eh_frame (DWARF4 §6.4.2) constants. */
enum
{
    EH_CIE_ID = 0,
    EH_VERSION = 1,
    EH_AUG_Z = 'z',
    EH_LEN64 = 0xffffffffu,
};

enum
{
    CFA_nop = 0x00,
    CFA_advance_loc1 = 0x02,
    CFA_advance_loc2 = 0x03,
    CFA_advance_loc4 = 0x04,
    CFA_def_cfa = 0x0c,
    CFA_def_cfa_register = 0x0d,
    CFA_def_cfa_offset = 0x0e,
    CFA_def_cfa_expression = 0x0f,
    CFA_ADVANCE_LOC_MASK = 0x40,
    CFA_OFFSET_MASK = 0x80,
};

/* Fixed ELF64 header/section-header/rela field offsets (little-endian). */
enum
{
    ELF_EI_CLASS = 4,
    ELF_EI_DATA = 5,
    ELF_EI_VERSION = 6,
    ELF_CLASS64 = 2,
    ELF_LSB = 1,
    ELF_EV_CURRENT = 1,
    ELF_MAGIC_LEN = 4,
    EHDR_SZ = 64,
    EHDR_SHOFF = 40,
    EHDR_SHNUM = 60,
    EHDR_SHSTRNDX = 62,
    SHDR_SZ = 64,
    SHDR_NAME = 0,
    SHDR_OFFSET = 24,
    SHDR_SIZE = 32,
    RELA_SZ = 24,
    RELA_OFF = 0,
    RELA_INFO = 8,
    RELA_ADDEND = 16,
};

static void dc_err(DwarfCheck *out, const char *msg)
{
    if (!out->err)
    {
        out->err = msg;
    }
}

static void *dc_alloc(DwarfCheck *out, size_t sz)
{
    return arena_alloc(out->arena, sz, sizeof(void *));
}

/* Record an error and return the bad-position sentinel. */
static size_t fail_pos(DwarfCheck *out, const char *msg)
{
    dc_err(out, msg);
    return (size_t) -1;
}

/* little-endian word reads (the only endianness elf.c emits) */

static u16 rd16(const u8 *p)
{
    return (u16) (p[0] | (u16) p[1] << 8);
}

static u32 rd32(const u8 *p)
{
    return (u32) p[0] | (u32) p[1] << 8 | (u32) p[2] << 16 | (u32) p[3] << 24;
}

static u64 rd64(const u8 *p)
{
    return (u64) rd32(p) | (u64) rd32(p + 4) << 32;
}

/* LEB128 readers */

static bool read_uleb(const u8 *p, size_t len, size_t *pos, u64 *out)
{
    u64 v = 0;
    unsigned shift = 0;
    for (;;)
    {
        if (*pos >= len || shift > 63)
        {
            return false;
        }
        u8 b = p[(*pos)++];
        v |= (u64) (b & 0x7f) << shift;
        if (!(b & LEB_CONT))
        {
            break;
        }
        shift += 7;
    }
    *out = v;
    return true;
}

static bool read_sleb(const u8 *p, size_t len, size_t *pos, i64 *out)
{
    i64 v = 0;
    unsigned shift = 0;
    u8 b;
    for (;;)
    {
        if (*pos >= len || shift > 63)
        {
            return false;
        }
        b = p[(*pos)++];
        v |= (i64) (b & 0x7f) << shift;
        shift += 7;
        if (!(b & LEB_CONT))
        {
            break;
        }
    }
    if (shift < 64 && (b & LEB_SIGN))
    {
        v |= -((i64) 1 << shift);
    }
    *out = v;
    return true;
}

static bool skip_cstr(const u8 *p, size_t end, size_t *pos)
{
    while (*pos < end && p[*pos])
    {
        (*pos)++;
    }
    if (*pos >= end)
    {
        return false;
    }
    (*pos)++;
    return true;
}

/* The single relocation whose `offset` matches `slot`, if any. */
static bool reloc_at(Vec *relas, u64 slot, i64 *addend)
{
    for (size_t i = 0; i < vec_size(relas); i++)
    {
        DwarfCheckReloc *r = (DwarfCheckReloc *) vec_get(relas, i);
        if (r->offset == slot)
        {
            *addend = r->addend;
            return true;
        }
    }
    return false;
}

/* True when a relocation covers `slot` with a symbol index in [lo, hi]. */
static bool reloc_covers(Vec *relas, u64 slot, u32 lo, u32 hi)
{
    for (size_t i = 0; i < vec_size(relas); i++)
    {
        DwarfCheckReloc *r = (DwarfCheckReloc *) vec_get(relas, i);
        if (r->offset == slot)
        {
            return r->sym >= lo && r->sym <= hi;
        }
    }
    return false;
}

/* ELF object loading */

typedef struct
{
    u64 off;
    u64 size;
} Sec;

static bool elf_shdr(const u8 *img, size_t len, u64 shoff, u32 i, Sec *out)
{
    u64 base = shoff + (u64) i * SHDR_SZ;
    if (base + SHDR_SZ > len)
    {
        return false;
    }
    out->off = rd64(img + base + SHDR_OFFSET);
    out->size = rd64(img + base + SHDR_SIZE);
    return out->off + out->size <= len;
}

static void rela_parse(DwarfCheck *out, const u8 *p, size_t size, Vec *dst)
{
    for (size_t off = 0; off + RELA_SZ <= size; off += RELA_SZ)
    {
        DwarfCheckReloc *r = dc_alloc(out, sizeof(DwarfCheckReloc));
        r->offset = rd64(p + off + RELA_OFF);
        r->addend = (i64) rd64(p + off + RELA_ADDEND);
        r->sym = (u32) (rd64(p + off + RELA_INFO) >> 32);
        vec_push(dst, r);
    }
}

static void sec_route(DwarfCheck *out, const u8 *img, const char *name, Sec s)
{
    const u8 *p = img + s.off;
    size_t n = (size_t) s.size;
    if (strcmp(name, ".text") == 0)
    {
        out->text = p;
        out->text_len = n;
    }
    else if (strcmp(name, ".debug_info") == 0)
    {
        out->debug_info = p;
        out->debug_info_len = n;
    }
    else if (strcmp(name, ".debug_abbrev") == 0)
    {
        out->debug_abbrev = p;
        out->debug_abbrev_len = n;
    }
    else if (strcmp(name, ".debug_line") == 0)
    {
        out->debug_line = p;
        out->debug_line_len = n;
    }
    else if (strcmp(name, ".debug_str") == 0)
    {
        out->debug_str = p;
        out->debug_str_len = n;
    }
    else if (strcmp(name, ".debug_loc") == 0)
    {
        out->debug_loc = p;
        out->debug_loc_len = n;
    }
    else if (strcmp(name, ".eh_frame") == 0)
    {
        out->eh_frame = p;
        out->eh_frame_len = n;
    }
    else if (strcmp(name, ".rela.debug_info") == 0)
    {
        rela_parse(out, p, n, out->rela_info);
    }
    else if (strcmp(name, ".rela.debug_line") == 0)
    {
        rela_parse(out, p, n, out->rela_line);
    }
    else if (strcmp(name, ".rela.eh_frame") == 0)
    {
        rela_parse(out, p, n, out->rela_eh);
    }
}

void dwarf_check_load(const char *path, DwarfCheck *out, Arena *arena)
{
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    out->err = NULL;
    out->rela_info = vec_new(arena);
    out->rela_line = vec_new(arena);
    out->rela_eh = vec_new(arena);

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        dc_err(out, "cannot open object");
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        dc_err(out, "cannot size object");
        return;
    }
    long size = ftell(f);
    if (size < EHDR_SZ || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        dc_err(out, "object too small");
        return;
    }
    u8 *img = arena_alloc(arena, (size_t) size, 1);
    if (fread(img, 1, (size_t) size, f) != (size_t) size)
    {
        fclose(f);
        dc_err(out, "cannot read object");
        return;
    }
    fclose(f);

    size_t len = (size_t) size;
    if (len < EHDR_SZ || memcmp(img,
                                "\x7f"
                                "ELF",
                                4) != 0)
    {
        dc_err(out, "no ELF magic");
        return;
    }
    if (img[ELF_EI_CLASS] != ELF_CLASS64 || img[ELF_EI_DATA] != ELF_LSB ||
        img[ELF_EI_VERSION] != ELF_EV_CURRENT)
    {
        dc_err(out, "not little-endian ELF64");
        return;
    }
    u64 shoff = rd64(img + EHDR_SHOFF);
    u16 shnum = rd16(img + EHDR_SHNUM);
    u16 shstrndx = rd16(img + EHDR_SHSTRNDX);
    if (shnum == 0 || shstrndx >= shnum || shoff + (u64) shnum * SHDR_SZ > len)
    {
        dc_err(out, "bad section table");
        return;
    }
    Sec shstrsec;
    if (!elf_shdr(img, len, shoff, shstrndx, &shstrsec))
    {
        dc_err(out, "bad shstrtab");
        return;
    }
    const u8 *shstr = img + shstrsec.off;

    for (u32 i = 0; i < shnum; i++)
    {
        u64 base = shoff + (u64) i * SHDR_SZ;
        u32 name = rd32(img + base + SHDR_NAME);
        Sec s;
        if (!elf_shdr(img, len, shoff, i, &s))
        {
            continue;
        }
        if (name < shstrsec.size)
        {
            sec_route(out, img, (const char *) (shstr + name), s);
        }
    }
}

void dwarf_check_from_buffers(DwarfCheck *out, Arena *arena, const u8 *info, size_t info_len,
                              const u8 *abbrev, size_t abbrev_len, const u8 *line, size_t line_len,
                              const u8 *loc, size_t loc_len, const u8 *eh, size_t eh_len,
                              const u8 *text, size_t text_len, Vec *rela_info, Vec *rela_line,
                              Vec *rela_eh)
{
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    out->err = NULL;
    out->debug_info = info;
    out->debug_info_len = info_len;
    out->debug_abbrev = abbrev;
    out->debug_abbrev_len = abbrev_len;
    out->debug_line = line;
    out->debug_line_len = line_len;
    out->debug_loc = loc;
    out->debug_loc_len = loc_len;
    out->eh_frame = eh;
    out->eh_frame_len = eh_len;
    out->text = text;
    out->text_len = text_len;
    out->rela_info = rela_info;
    out->rela_line = rela_line;
    out->rela_eh = rela_eh;
}

/* .debug_line */

/* Program registers plus the header fields the decoder advances. */
typedef struct
{
    u64 addr;
    i64 line;
    u32 file;
    u8 min_inst;
    i8 line_base;
    u8 line_range;
    u8 opcode_base;
} LineRegs;

typedef struct
{
    DwarfCheck *out;
    Arena *arena;
    const u8 *p; /* .debug_line bytes */
    size_t unit_end;
    size_t prog; /* first program byte after the header */
    DwarfCheckLines *l;
    LineRegs regs;
} LineCtx;

static void push_row(DwarfCheckLines *l, Arena *a, const LineRegs *r, bool end_seq)
{
    DwarfCheckRow *row = arena_alloc(a, sizeof(DwarfCheckRow), sizeof(void *));
    row->addr = r->addr;
    row->line = r->line;
    row->file = r->file;
    row->end_seq = end_seq;
    vec_push(l->rows, row);
}

static bool line_files(const u8 *p, size_t prog, size_t *pos)
{
    for (;;)
    {
        if (*pos >= prog)
        {
            return false;
        }
        if (p[*pos] == 0) /* empty name terminates the list */
        {
            (*pos)++;
            return true;
        }
        if (!skip_cstr(p, prog, pos))
        {
            return false;
        }
        u64 dir, mtime, fsize;
        if (!read_uleb(p, prog, pos, &dir) || !read_uleb(p, prog, pos, &mtime) ||
            !read_uleb(p, prog, pos, &fsize))
        {
            return false;
        }
    }
}

static bool line_read_header(LineCtx *c)
{
    const u8 *p = c->p;
    size_t len = c->out->debug_line_len;
    if (len < sizeof(u32) + sizeof(u16) + sizeof(u32) + LINE_HDR_FIELDS)
    {
        dc_err(c->out, ".debug_line too short");
        return false;
    }
    c->unit_end = sizeof(u32) + (size_t) rd32(p);
    if (c->unit_end > len)
    {
        dc_err(c->out, ".debug_line unit overruns section");
        return false;
    }
    u16 version = rd16(p + sizeof(u32));
    u32 header_len = rd32(p + sizeof(u32) + sizeof(u16));
    size_t hdr_start = sizeof(u32) + sizeof(u16) + sizeof(u32);
    if (version < 2 || version > 4)
    {
        dc_err(c->out, ".debug_line version outside 2..4");
        return false;
    }
    c->regs.min_inst = p[hdr_start + LINE_OP_MIN];
    c->regs.line_base = (i8) p[hdr_start + LINE_BASE];
    c->regs.line_range = p[hdr_start + LINE_RANGE];
    c->regs.opcode_base = p[hdr_start + LINE_OPCODE_BASE];
    size_t prog = hdr_start + header_len;
    if (c->regs.opcode_base == 0 || prog > c->unit_end)
    {
        dc_err(c->out, ".debug_line header overruns unit");
        return false;
    }
    size_t pos = hdr_start + LINE_HDR_FIELDS + c->regs.opcode_base - 1;
    if (!skip_cstr(p, prog, &pos) && pos != prog)
    {
        dc_err(c->out, ".debug_line include_directories unterminated");
        return false;
    }
    if (!line_files(p, prog, &pos))
    {
        dc_err(c->out, ".debug_line file_names unterminated");
        return false;
    }
    c->regs.addr = 0;
    c->regs.line = 1;
    c->regs.file = 1;
    c->prog = prog;
    return true;
}

/* One DW_LNE_* extended opcode at `pos` (its sub-opcode byte). */
static size_t line_extended(LineCtx *c, size_t pos)
{
    u64 elen;
    if (!read_uleb(c->p, c->unit_end, &pos, &elen) || elen == 0)
    {
        return fail_pos(c->out, ".debug_line extended opcode trashed");
    }
    size_t end = pos + (size_t) elen;
    if (end > c->unit_end)
    {
        return fail_pos(c->out, ".debug_line extended opcode overruns");
    }
    u8 ext = c->p[pos];
    if (ext == LNE_end_sequence)
    {
        push_row(c->l, c->arena, &c->regs, true);
        /* end_sequence restores the initial state, resetting each function's line numbers. */
        c->regs.line = 1;
        c->regs.file = 1;
    }
    else if (ext == LNE_set_address)
    {
        /* The slot bytes are zero; the relocation addend carries the address. */
        size_t slot = pos + 1;
        if (elen != 1 + ADDR_BYTES || rd64(c->p + slot) != 0)
        {
            return fail_pos(c->out, ".debug_line set_address slot bad");
        }
        i64 resolved;
        if (!reloc_at(c->out->rela_line, slot, &resolved))
        {
            return fail_pos(c->out, ".debug_line set_address has no covering relocation");
        }
        DwarfCheckSetAddr *sa = arena_alloc(c->arena, sizeof(DwarfCheckSetAddr), sizeof(void *));
        sa->slot = (u32) slot;
        sa->value = (u64) resolved;
        vec_push(c->l->set_addresses, sa);
        c->regs.addr = (u64) resolved;
    }
    return end;
}

/* One DW_LNS_* standard opcode; returns the next position. */
static size_t line_standard(LineCtx *c, size_t pos, u8 op)
{
    const u8 *p = c->p;
    switch (op)
    {
        case LNS_copy:
            push_row(c->l, c->arena, &c->regs, false);
            break;
        case LNS_advance_pc:
        {
            u64 adv;
            if (!read_uleb(p, c->unit_end, &pos, &adv))
            {
                return fail_pos(c->out, ".debug_line advance_pc trashed");
            }
            c->regs.addr += adv;
            break;
        }
        case LNS_advance_line:
        {
            i64 dl;
            if (!read_sleb(p, c->unit_end, &pos, &dl))
            {
                return fail_pos(c->out, ".debug_line advance_line trashed");
            }
            c->regs.line += dl;
            break;
        }
        case LNS_set_file:
        {
            u64 fl;
            if (!read_uleb(p, c->unit_end, &pos, &fl))
            {
                return fail_pos(c->out, ".debug_line set_file trashed");
            }
            c->regs.file = (u32) fl;
            break;
        }
        case LNS_set_column:
        {
            u64 col;
            if (!read_uleb(p, c->unit_end, &pos, &col))
            {
                return fail_pos(c->out, ".debug_line set_column trashed");
            }
            break;
        }
        case LNS_negate_stmt:
        case LNS_set_basic_block:
        case LNS_set_prologue_end:
        case LNS_set_epilogue_begin:
            break;
        case LNS_const_add_pc:
            c->regs.addr += (u64) c->regs.min_inst *
                            ((LINE_LAST_OPCODE - c->regs.opcode_base) / c->regs.line_range);
            break;
        case LNS_fixed_advance_pc:
            c->regs.addr += rd16(p + pos);
            pos += sizeof(u16);
            break;
        case LNS_set_isa:
        {
            u64 isa;
            if (!read_uleb(p, c->unit_end, &pos, &isa))
            {
                return fail_pos(c->out, ".debug_line set_isa trashed");
            }
            break;
        }
        default:
            return fail_pos(c->out, ".debug_line unknown standard opcode");
    }
    return pos;
}

/* Adjust the line registers via the special-opcode delta and emit a row. */
static void line_special(LineCtx *c, u8 op)
{
    LineRegs *r = &c->regs;
    u32 adj = op - r->opcode_base;
    r->addr += (u64) r->min_inst * (adj / r->line_range);
    r->line += (i64) r->line_base + (i64) (adj % r->line_range);
    push_row(c->l, c->arena, r, false);
}

DwarfCheckLines *dwarf_check_lines(DwarfCheck *out, Arena *arena)
{
    DwarfCheckLines *l = arena_alloc(arena, sizeof(DwarfCheckLines), sizeof(void *));
    l->rows = vec_new(arena);
    l->set_addresses = vec_new(arena);

    LineCtx c = {
        .out = out,
        .arena = arena,
        .p = out->debug_line,
        .l = l,
    };
    if (!line_read_header(&c))
    {
        return NULL;
    }
    size_t pos = c.prog;
    while (pos < c.unit_end)
    {
        u8 op = c.p[pos++];
        if (op == 0)
        {
            pos = line_extended(&c, pos);
        }
        else if (op < c.regs.opcode_base)
        {
            pos = line_standard(&c, pos, op);
        }
        else
        {
            line_special(&c, op);
        }
        if (pos == (size_t) -1)
        {
            return NULL;
        }
    }
    return l;
}

bool dwarf_check_line_relocs_covered(DwarfCheck *out, const DwarfCheckLines *lines, u32 sym_lo,
                                     u32 sym_hi)
{
    if (vec_size(out->rela_line) != vec_size(lines->set_addresses))
    {
        dc_err(out, ".rela.debug_line count != set_address count");
        return false;
    }
    for (size_t i = 0; i < vec_size(out->rela_line); i++)
    {
        DwarfCheckReloc *r = (DwarfCheckReloc *) vec_get(out->rela_line, i);
        DwarfCheckSetAddr *sa = (DwarfCheckSetAddr *) vec_get(lines->set_addresses, i);
        if (r->offset != sa->slot)
        {
            dc_err(out, ".rela.debug_line offset misses a set_address slot");
            return false;
        }
        if (r->sym < sym_lo || r->sym > sym_hi)
        {
            dc_err(out, ".rela.debug_line symbol outside section symbols");
            return false;
        }
        if (r->addend < 0 || (u64) r->addend >= out->text_len)
        {
            dc_err(out, ".rela.debug_line addend outside .text");
            return false;
        }
    }
    return true;
}

/* .debug_info: abbrev table + DIE tree */

typedef struct
{
    u32 attr;
    u32 form;
} AttrSpec;

typedef struct
{
    u32 code;
    u32 tag;
    u8 children;
    Vec *specs; /* Vec<AttrSpec*> */
} Abbrev;

typedef struct
{
    DwarfCheck *out;
    const u8 *p;
    size_t end;
    Arena *a;
    Vec *abbrevs;
    Vec *dies;
    Vec *refs;
} InfoCtx;

#define INFO_MAX_DEPTH 512

static const Abbrev *abbrev_lookup(InfoCtx *ctx, u32 code)
{
    for (size_t i = 0; i < vec_size(ctx->abbrevs); i++)
    {
        Abbrev *ab = (Abbrev *) vec_get(ctx->abbrevs, i);
        if (ab->code == code)
        {
            return ab;
        }
    }
    return NULL;
}

Vec *dwarf_check_locs(DwarfCheck *out, u64 off, Arena *arena)
{
    Vec *ranges = vec_new(arena);
    const u8 *p = out->debug_loc;
    size_t len = out->debug_loc_len;
    size_t q = (size_t) off;
    while (p && q + 2 * ADDR_BYTES + 2 <= len)
    {
        u64 begin = rd64(p + q);
        u64 end = rd64(p + q + ADDR_BYTES);
        q += 2 * ADDR_BYTES;
        if (begin == 0 && end == 0)
        {
            break; /* end-of-list marker */
        }
        u16 elen = rd16(p + q);
        q += 2;
        if (q + elen > len)
        {
            dc_err(out, "debug_loc expression overruns the section");
            return NULL;
        }
        DwarfCheckLocRange *r = arena_alloc(arena, sizeof(DwarfCheckLocRange), sizeof(void *));
        r->begin = begin;
        r->end = end;
        r->expr = p + q;
        r->expr_len = elen;
        vec_push(ranges, r);
        q += elen;
    }
    return ranges;
}

/* Resolve a .debug_loc offset to the first entry's expression bytes. */
static bool loc_first_expr(DwarfCheck *out, u64 off, const u8 **expr, u32 *expr_len)
{
    Vec *ranges = dwarf_check_locs(out, off, out->arena);
    if (!ranges || vec_size(ranges) == 0)
    {
        return false;
    }
    DwarfCheckLocRange *first = (DwarfCheckLocRange *) vec_get(ranges, 0);
    *expr = first->expr;
    *expr_len = first->expr_len;
    return true;
}

/* Read one attribute value into *attr; *next advances past it. False on error. */
static bool info_read_attr(InfoCtx *ctx, AttrSpec *spec, size_t pos, DwarfCheckAttr **attr,
                           size_t *next)
{
    DwarfCheckAttr *a = arena_alloc(ctx->a, sizeof(DwarfCheckAttr), sizeof(void *));
    a->attr = spec->attr;
    a->loc = NULL;
    a->loc_len = 0;
    a->str = NULL;
    a->value_off = (u32) pos;
    switch (spec->form)
    {
        case FORM_addr:
            if (pos + ADDR_BYTES > ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_ADDR;
            a->addr = rd64(ctx->p + pos);
            pos += ADDR_BYTES;
            break;
        case FORM_data1:
            if (pos >= ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_NUM;
            a->num = ctx->p[pos];
            pos += 1;
            break;
        case FORM_data2:
            if (pos + sizeof(u16) > ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_NUM;
            a->num = rd16(ctx->p + pos);
            pos += sizeof(u16);
            break;
        case FORM_data4:
            if (pos + sizeof(u32) > ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_NUM;
            a->num = rd32(ctx->p + pos);
            pos += sizeof(u32);
            break;
        case FORM_udata:
        {
            u64 v;
            if (!read_uleb(ctx->p, ctx->end, &pos, &v))
            {
                goto overrun;
            }
            a->kind = DW_ATTR_NUM;
            a->num = v;
            break;
        }
        case FORM_flag:
            if (pos >= ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_NUM;
            a->num = ctx->p[pos];
            pos += 1;
            break;
        case FORM_string:
            a->kind = DW_ATTR_STR;
            a->str = (const char *) (ctx->p + pos);
            if (!skip_cstr(ctx->p, ctx->end, &pos))
            {
                goto overrun;
            }
            break;
        case FORM_ref4:
            if (pos + sizeof(u32) > ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_REF;
            a->ref = rd32(ctx->p + pos);
            pos += sizeof(u32);
            {
                u64 *r = arena_alloc(ctx->a, sizeof(u64), sizeof(void *));
                *r = a->ref;
                vec_push(ctx->refs, r);
            }
            break;
        case FORM_exprloc:
        {
            u64 elen;
            if (!read_uleb(ctx->p, ctx->end, &pos, &elen) || pos + elen > ctx->end)
            {
                goto overrun;
            }
            a->kind = DW_ATTR_LOC;
            a->loc = ctx->p + pos;
            a->loc_len = (u32) elen;
            pos += (size_t) elen;
            break;
        }
        case FORM_sec_offset:
        {
            if (pos + sizeof(u32) > ctx->end)
            {
                goto overrun;
            }
            u32 off = rd32(ctx->p + pos);
            pos += sizeof(u32);
            a->kind = DW_ATTR_NUM;
            a->num = off;
            /* A location attr is a .debug_loc offset; expose its first entry's expr. */
            const u8 *expr;
            u32 expr_len;
            if (a->attr == DW_AT_location && loc_first_expr(ctx->out, off, &expr, &expr_len))
            {
                a->kind = DW_ATTR_LOC;
                a->loc = expr;
                a->loc_len = expr_len;
            }
            break;
        }
        default:
            dc_err(ctx->out, "unknown attribute form in DIE");
            return false;
    }
    *attr = a;
    *next = pos;
    return true;
overrun:
    dc_err(ctx->out, "attribute overruns .debug_info");
    return false;
}

static size_t info_children(InfoCtx *ctx, size_t pos, u32 depth)
{
    if (depth > INFO_MAX_DEPTH)
    {
        dc_err(ctx->out, "DIE nesting too deep");
        return (size_t) -1;
    }
    while (pos < ctx->end)
    {
        size_t die_off = pos;
        u64 code;
        if (!read_uleb(ctx->p, ctx->end, &pos, &code))
        {
            dc_err(ctx->out, "DIE abbrev code trashed");
            return (size_t) -1;
        }
        if (code == 0) /* null DIE ends the enclosing children list */
        {
            return pos;
        }
        const Abbrev *ab = abbrev_lookup(ctx, (u32) code);
        if (!ab)
        {
            dc_err(ctx->out, "DIE references unknown abbrev code");
            return (size_t) -1;
        }
        DwarfCheckDie *die = arena_alloc(ctx->a, sizeof(DwarfCheckDie), sizeof(void *));
        die->off = (u32) die_off;
        die->tag = ab->tag;
        die->has_children = ab->children != 0;
        die->attrs = vec_new(ctx->a);
        vec_push(ctx->dies, die);

        for (size_t i = 0; i < vec_size(ab->specs); i++)
        {
            AttrSpec *spec = (AttrSpec *) vec_get(ab->specs, i);
            DwarfCheckAttr *attr;
            if (!info_read_attr(ctx, spec, pos, &attr, &pos))
            {
                return (size_t) -1;
            }
            vec_push(die->attrs, attr);
        }
        if (die->has_children)
        {
            pos = info_children(ctx, pos, depth + 1);
            if (pos == (size_t) -1)
            {
                return (size_t) -1;
            }
        }
    }
    return pos;
}

/* Parse the whole .debug_abbrev table; NULL (with err) on a bad stream. */
static Vec *abbrev_table(DwarfCheck *out, const u8 *p, size_t len, Arena *a)
{
    Vec *abbrevs = vec_new(a);
    size_t pos = 0;
    for (;;)
    {
        u64 code;
        if (!read_uleb(p, len, &pos, &code))
        {
            dc_err(out, "abbrev table overruns section");
            return NULL;
        }
        if (code == 0)
        {
            break;
        }
        u64 tag, children;
        if (!read_uleb(p, len, &pos, &tag) || !read_uleb(p, len, &pos, &children))
        {
            dc_err(out, "abbrev entry trashed");
            return NULL;
        }
        Abbrev *e = arena_alloc(a, sizeof(Abbrev), sizeof(void *));
        e->code = (u32) code;
        e->tag = (u32) tag;
        e->children = (u8) children;
        e->specs = vec_new(a);
        vec_push(abbrevs, e);
        for (;;)
        {
            u64 at, fm;
            if (!read_uleb(p, len, &pos, &at) || !read_uleb(p, len, &pos, &fm))
            {
                dc_err(out, "abbrev attribute trashed");
                return NULL;
            }
            if (at == 0 && fm == 0)
            {
                break;
            }
            AttrSpec *s = arena_alloc(a, sizeof(AttrSpec), sizeof(void *));
            s->attr = (u32) at;
            s->form = (u32) fm;
            vec_push(e->specs, s);
        }
    }
    return abbrevs;
}

DwarfCheckInfo *dwarf_check_info(DwarfCheck *out, Arena *arena)
{
    if (!out->debug_info || !out->debug_abbrev)
    {
        dc_err(out, "missing .debug_info/.debug_abbrev");
        return NULL;
    }
    const u8 *ab = out->debug_abbrev;
    size_t ablen = out->debug_abbrev_len;
    Vec *abbrevs = abbrev_table(out, ab, ablen, arena);
    if (!abbrevs)
    {
        return NULL;
    }
    size_t pos;

    const u8 *p = out->debug_info;
    size_t len = out->debug_info_len;
    if (len < 11)
    {
        dc_err(out, ".debug_info too short");
        return NULL;
    }
    DwarfCheckInfo *info = arena_alloc(arena, sizeof(DwarfCheckInfo), sizeof(void *));
    info->arena = arena;
    info->dies = vec_new(arena);
    info->refs = vec_new(arena);

    size_t unit_len = rd32(p);
    size_t unit_end = sizeof(u32) + unit_len;
    if (unit_end > len)
    {
        dc_err(out, ".debug_info unit overruns section");
        return NULL;
    }
    info->version = rd16(p + sizeof(u32));
    info->abbrev_off = rd32(p + sizeof(u32) + sizeof(u16));
    if (info->abbrev_off > ablen)
    {
        dc_err(out, "abbrev offset out of range");
        return NULL;
    }
    info->addr_size = p[sizeof(u32) + sizeof(u16) + sizeof(u32)];
    pos = sizeof(u32) + sizeof(u16) + sizeof(u32) + 1;

    InfoCtx ctx = {
        .out = out,
        .p = p,
        .end = unit_end,
        .a = arena,
        .abbrevs = abbrevs,
        .dies = info->dies,
        .refs = info->refs,
    };
    if (info_children(&ctx, pos, 0) == (size_t) -1)
    {
        return NULL;
    }

    /* Every ref4 must land on a DIE header offset. */
    for (size_t i = 0; i < vec_size(ctx.refs); i++)
    {
        u64 target = *(u64 *) vec_get(ctx.refs, i);
        bool found = false;
        for (size_t j = 0; j < vec_size(ctx.dies) && !found; j++)
        {
            DwarfCheckDie *d = (DwarfCheckDie *) vec_get(ctx.dies, j);
            found = d->off == (u32) target;
        }
        if (!found)
        {
            dc_err(out, "ref4 points outside the DIE tree");
            return NULL;
        }
    }
    return info;
}

bool dwarf_check_info_relocs_covered(DwarfCheck *out, const DwarfCheckInfo *info, u32 sym_lo,
                                     u32 sym_hi)
{
    Vec *slots = vec_new(out->arena);
    for (size_t i = 0; i < vec_size(info->dies); i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        for (size_t j = 0; j < vec_size(d->attrs); j++)
        {
            DwarfCheckAttr *a = (DwarfCheckAttr *) vec_get(d->attrs, j);
            u64 slot;
            if (a->kind == DW_ATTR_ADDR && a->addr == 0)
            {
                slot = a->value_off;
            }
            else if (a->kind == DW_ATTR_LOC && a->loc_len == 1 + ADDR_BYTES &&
                     a->loc[0] == DW_OP_addr && rd64(a->loc + 1) == 0)
            {
                slot = (u64) (a->loc - out->debug_info) + 1;
            }
            else
            {
                continue;
            }
            u64 *s = arena_alloc(out->arena, sizeof(u64), sizeof(void *));
            *s = slot;
            vec_push(slots, s);
        }
    }

    for (size_t i = 0; i < vec_size(slots); i++)
    {
        u64 slot = *(u64 *) vec_get(slots, i);
        if (!reloc_covers(out->rela_info, slot, sym_lo, sym_hi))
        {
            dc_err(out, "uncovered address slot in .debug_info");
            return false;
        }
    }
    return true;
}

/* helpers the tests share */

size_t dwarf_check_sleb128(const u8 *buf, size_t len, i64 *out)
{
    i64 v = 0;
    unsigned shift = 0;
    u8 b;
    size_t pos = 0;
    for (;;)
    {
        if (pos >= len || shift > 63)
        {
            return 0;
        }
        b = buf[pos++];
        v |= (i64) (b & 0x7f) << shift;
        shift += 7;
        if (!(b & LEB_CONT))
        {
            break;
        }
    }
    if (shift < 64 && (b & LEB_SIGN))
    {
        v |= -((i64) 1 << shift);
    }
    *out = v;
    return pos;
}

DwarfCheckAttr *dwarf_check_attr(const DwarfCheckDie *die, u32 attr)
{
    for (size_t i = 0; i < vec_size(die->attrs); i++)
    {
        DwarfCheckAttr *a = (DwarfCheckAttr *) vec_get(die->attrs, i);
        if (a->attr == attr)
        {
            return a;
        }
    }
    return NULL;
}

Vec *dwarf_check_dies_by_tag(const DwarfCheckInfo *info, u32 tag)
{
    Vec *out = vec_new(info->arena);
    for (size_t i = 0; i < vec_size(info->dies); i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        if (d->tag == tag)
        {
            vec_push(out, d);
        }
    }
    return out;
}

DwarfCheckDie *dwarf_check_die_named(const DwarfCheckInfo *info, const char *name)
{
    for (size_t i = 0; i < vec_size(info->dies); i++)
    {
        DwarfCheckDie *d = (DwarfCheckDie *) vec_get(info->dies, i);
        DwarfCheckAttr *a = dwarf_check_attr(d, DW_AT_name);
        if (a && a->kind == DW_ATTR_STR && strcmp(a->str, name) == 0)
        {
            return d;
        }
    }
    return NULL;
}

/* .eh_frame */

/* Skip a 'z' augmentation: uleb length followed by that many bytes. */
static bool skip_aug_data(DwarfCheck *out, const u8 *p, size_t end, size_t *pos)
{
    u64 len;
    if (!read_uleb(p, end, pos, &len) || *pos + len > end)
    {
        dc_err(out, "eh_frame augmentation data trashed");
        return false;
    }
    *pos += (size_t) len;
    return true;
}

static bool eh_decode_ops(DwarfCheck *out, const u8 *p, size_t from, size_t to, Vec *ops)
{
    size_t pos = from;
    while (pos < to)
    {
        u8 op = p[pos++];
        u64 *rec = arena_alloc(out->arena, sizeof(u64), sizeof(void *));
        *rec = op;
        vec_push(ops, rec);

        if (op == CFA_nop || (op & CFA_ADVANCE_LOC_MASK) == CFA_ADVANCE_LOC_MASK)
        {
            continue; /* no operand */
        }
        if ((op & CFA_OFFSET_MASK) == CFA_OFFSET_MASK)
        {
            u64 v;
            if (!read_uleb(p, to, &pos, &v))
            {
                dc_err(out, "CFA offset operand trashed");
                return false;
            }
            continue;
        }
        switch (op)
        {
            case CFA_advance_loc1:
            case CFA_advance_loc2:
            case CFA_advance_loc4:
            {
                size_t n = op == CFA_advance_loc1 ? 1 : (op == CFA_advance_loc2 ? 2 : 4);
                if (pos + n > to)
                {
                    dc_err(out, "CFA advance_loc operand overruns");
                    return false;
                }
                pos += n;
                break;
            }
            case CFA_def_cfa:
            case CFA_def_cfa_register:
            case CFA_def_cfa_offset:
            {
                u64 v;
                if (!read_uleb(p, to, &pos, &v) ||
                    (op == CFA_def_cfa && !read_uleb(p, to, &pos, &v)))
                {
                    dc_err(out, "CFA def_cfa operand trashed");
                    return false;
                }
                break;
            }
            case CFA_def_cfa_expression:
            {
                u64 elen;
                if (!read_uleb(p, to, &pos, &elen) || pos + elen > to)
                {
                    dc_err(out, "CFA def_cfa_expression trashed");
                    return false;
                }
                pos += (size_t) elen;
                break;
            }
            default:
                dc_err(out, "unknown CFA opcode");
                return false;
        }
    }
    return true;
}

/* CIE header fields; returns the first CFA instruction or -1. */
static size_t cie_fields(DwarfCheck *out, const u8 *p, size_t q, size_t end, bool *has_z)
{
    if (q >= end || p[q] != EH_VERSION)
    {
        return fail_pos(out, "CIE version is not 1");
    }
    q++;
    const char *aug = (const char *) (p + q);
    if (!skip_cstr(p, end, &q))
    {
        return fail_pos(out, "CIE augmentation string unterminated");
    }
    *has_z = strchr(aug, EH_AUG_Z) != NULL;
    u64 align;
    i64 dalign;
    if (!read_uleb(p, end, &q, &align) || !read_sleb(p, end, &q, &dalign))
    {
        return fail_pos(out, "CIE alignment factors trashed");
    }
    if (q >= end)
    {
        return fail_pos(out, "CIE missing return-address register");
    }
    q++;
    if (*has_z && !skip_aug_data(out, p, end, &q))
    {
        return (size_t) -1;
    }
    return q;
}

/* FDE pc fields; fills `e` and returns the first CFA instruction or -1. */
static size_t fde_fields(DwarfCheck *out, const u8 *p, size_t content, size_t end, bool has_z,
                         DwarfCheckEhEntry *e)
{
    size_t slot = content + sizeof(u32);
    e->initial_slot = (u32) slot;
    /* initial_location is a zero + RELA addend, like the other slots. */
    if (rd64(p + slot) != 0)
    {
        return fail_pos(out, "FDE initial_location slot is not zero");
    }
    i64 begin;
    if (!reloc_at(out->rela_eh, slot, &begin))
    {
        return fail_pos(out, "FDE initial_location has no covering relocation");
    }
    e->fde_begin = (u64) begin;
    e->fde_range = rd64(p + content + sizeof(u32) + sizeof(u64));
    size_t q = content + sizeof(u32) + 2 * sizeof(u64);
    if (has_z && !skip_aug_data(out, p, end, &q))
    {
        return (size_t) -1;
    }
    return q;
}

DwarfCheckEh *dwarf_check_eh(DwarfCheck *out, Arena *arena)
{
    if (!out->eh_frame)
    {
        dc_err(out, "no .eh_frame section");
        return NULL;
    }
    const u8 *p = out->eh_frame;
    size_t len = out->eh_frame_len;
    DwarfCheckEh *eh = arena_alloc(arena, sizeof(DwarfCheckEh), sizeof(void *));
    eh->entries = vec_new(arena);
    eh->cfa_ops = vec_new(arena);
    eh->nfde = 0;

    size_t off = 0;
    bool has_z = false;
    while (off < len)
    {
        if (off + 2 * sizeof(u32) > len)
        {
            dc_err(out, ".eh_frame entry truncated");
            return NULL;
        }
        u32 entry_len = rd32(p + off);
        size_t content = off + sizeof(u32);
        size_t end = content + entry_len;
        if (entry_len == EH_LEN64 || end > len)
        {
            dc_err(out, ".eh_frame length field bad");
            return NULL;
        }

        u32 id = rd32(p + content);
        DwarfCheckEhEntry *e = arena_alloc(arena, sizeof(DwarfCheckEhEntry), sizeof(void *));
        e->offset = (u32) off;
        size_t inst; /* first CFA instruction */
        if (id == EH_CIE_ID)
        {
            inst = cie_fields(out, p, content + sizeof(u32), end, &has_z);
            e->is_cie = true;
        }
        else
        {
            inst = fde_fields(out, p, content, end, has_z, e);
            e->is_cie = false;
            e->cie_fde_pointer = id;
            eh->nfde++;
        }
        if (inst == (size_t) -1)
        {
            return NULL;
        }
        if (!eh_decode_ops(out, p, inst, end, eh->cfa_ops))
        {
            return NULL;
        }
        vec_push(eh->entries, e);
        off = end;
    }

    DwarfCheckEhEntry *first = (DwarfCheckEhEntry *) vec_get(eh->entries, 0);
    if (!first || !first->is_cie)
    {
        dc_err(out, ".eh_frame does not start with a CIE");
        return NULL;
    }
    return eh;
}
