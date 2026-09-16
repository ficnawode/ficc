#include "dwarf.h"
#include "util/assert.h"

#include <string.h>

/* ---- .debug_line (DWARF2 §6.2) ---- */

enum LineStdOp
{
    DW_LNS_copy = 1,
    DW_LNS_advance_pc,
    DW_LNS_advance_line,
    DW_LNS_set_file,
    DW_LNS_set_column,
    DW_LNS_negate_stmt,
    DW_LNS_set_basic_block,
    DW_LNS_const_add_pc,
    DW_LNS_fixed_advance_pc,
    DW_LNS_set_prologue_end,
    DW_LNS_set_epilogue_begin,
    DW_LNS_set_isa,
};

enum LineExtOp
{
    DW_LNE_end_sequence = 1,
    DW_LNE_set_address = 2,
};

enum DwarfTag
{
    DW_TAG_compile_unit = 0x11,
};

enum DwarfAttr
{
    DW_AT_name = 0x03,
    DW_AT_stmt_list = 0x10,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_language = 0x13,
    DW_AT_comp_dir = 0x1b,
    DW_AT_producer = 0x25,
};

enum DwarfForm
{
    DW_FORM_addr = 0x01,
    DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06,
    DW_FORM_string = 0x08,
};

enum
{
    DW_LINE_VERSION = 2,
    DW_INFO_VERSION = 4,
    DW_LINE_MIN_INST = 1,
    DW_LINE_IS_STMT = 1,
    DW_LINE_BASE = -5,
    DW_LINE_RANGE = 14,
    DW_LINE_OPCODE_BASE = 13,
    DW_LANG_C11 = 0x001d,
    DW_CHILDREN_YES = 1,
    DW_ADDRESS_SIZE = 8, /* SysV AMD64: 64-bit address slots for set_address */
};

/* unsigned LEB128 (DWARF4 §7.6): 7 bits per byte, high bit = more follows. */
void dwarf_uleb128(ByteBuf *b, u64 val)
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
void dwarf_sleb128(ByteBuf *b, i64 val)
{
    while (true)
    {
        u8 byte = (u8) (val & 0x7f);
        val >>= 7;
        bool sign_bit = (byte & 0x40) != 0;
        bool more = !((val == 0 && !sign_bit) || (val == -1 && sign_bit));
        if (more)
        {
            byte |= 0x80;
        }
        bytebuf_append(b, byte);
        if (!more)
        {
            break;
        }
    }
}

static void line_advance_pc(ByteBuf *b, u64 *addr, u64 target)
{
    if (target == *addr)
    {
        return;
    }
    bytebuf_append(b, DW_LNS_advance_pc);
    dwarf_uleb128(b, target - *addr);
    *addr = target;
}

static void line_advance_line(ByteBuf *b, i64 *line, i64 target)
{
    if (target == *line)
    {
        return;
    }
    bytebuf_append(b, DW_LNS_advance_line);
    dwarf_sleb128(b, target - *line);
    *line = target;
}

/* One row; prefer a one-byte special opcode, else advance/copy. */
static void line_row(ByteBuf *b, u64 *addr, i64 *line, u64 target_addr, i64 target_line)
{
    u64 daddr = target_addr - *addr;
    i64 dline = target_line - *line;
    i64 rel = dline - DW_LINE_BASE;
    if (rel >= 0 && rel < DW_LINE_RANGE)
    {
        u64 op = DW_LINE_OPCODE_BASE + daddr * DW_LINE_RANGE + (u64) rel;
        if (op <= 255)
        {
            bytebuf_append(b, (u8) op);
            *addr = target_addr;
            *line = target_line;
            return;
        }
    }
    line_advance_pc(b, addr, target_addr);
    line_advance_line(b, line, target_line);
    bytebuf_append(b, DW_LNS_copy);
}

static void line_end_sequence(ByteBuf *b)
{
    bytebuf_append(b, 0); /* opcode 0 = extended opcode follows */
    bytebuf_append(b, 1); /* extended opcode length */
    bytebuf_append(b, DW_LNE_end_sequence);
}

/* Set_address + rows + end_sequence for one function. */
static void line_func(ByteBuf *b, Vec *relocs, CodegenFunc *cf)
{
    bytebuf_append(b, 0);                          /* opcode 0 = extended opcode follows */
    bytebuf_append(b, (u8) (1 + DW_ADDRESS_SIZE)); /* extended opcode length */
    bytebuf_append(b, DW_LNE_set_address);
    size_t slot = bytebuf_len(b);
    bytebuf_append_u64(b, 0);
    DwarfReloc *rel = arena_alloc(b->arena, sizeof(DwarfReloc), sizeof(void *));
    rel->offset = (u64) slot;
    rel->addend = (i64) cf->offset;
    vec_push(relocs, rel);

    u64 addr = cf->offset;
    i64 line = 1;
    u32 last_row_line = 0; /* no row emitted yet: dedup same-line runs */
    ASSERT(cf->lines != NULL);
    size_t n = vec_size(cf->lines);
    for (size_t i = 0; i < n; i++)
    {
        LineEntry *le = (LineEntry *) vec_get(cf->lines, i);
        u64 target = cf->offset + le->offset;
        if (le->line != last_row_line)
        {
            line_row(b, &addr, &line, target, (i64) le->line);
            last_row_line = le->line;
        }
        else
        {
            addr = target; /* same-line run: extend the current row */
        }
    }

    size_t end = cf->offset + bytebuf_len(cf->bytes);
    line_advance_pc(b, &addr, (u64) end);
    line_end_sequence(b);
}

/* Header fields until (not including) the line program; lengths backpatched. */
static void line_header(ByteBuf *b, const char *compile_unit)
{
    bytebuf_append_u32(b, 0); /* unit length: patched in dwarf_build */
    bytebuf_append_u16(b, DW_LINE_VERSION);
    bytebuf_append_u32(b, 0); /* header length: patched in dwarf_build */
    bytebuf_append(b, DW_LINE_MIN_INST);
    bytebuf_append(b, DW_LINE_IS_STMT);
    bytebuf_append_i8(b, DW_LINE_BASE);
    bytebuf_append(b, DW_LINE_RANGE);
    bytebuf_append(b, DW_LINE_OPCODE_BASE);

    static const u8 std_oplen[DW_LINE_OPCODE_BASE - 1] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    for (size_t i = 0; i < sizeof(std_oplen); i++)
    {
        bytebuf_append(b, std_oplen[i]);
    }
    bytebuf_append(b, 0); /* empty include_directories */

    size_t name_len = strlen(compile_unit);
    bytebuf_append_bytes(b, (const u8 *) compile_unit, name_len);
    bytebuf_append(b, 0); /* end of file name */
    bytebuf_append(b, 0); /* dir index 0 */
    bytebuf_append(b, 0); /* mtime */
    bytebuf_append(b, 0); /* length */
    bytebuf_append(b, 0); /* end of file_names */
}

/* ---- .debug_info: a single DWARF4 compile-unit DIE ---- */

static void abbrev_emit(ByteBuf *b)
{
    dwarf_uleb128(b, 1); /* abbrev code */
    dwarf_uleb128(b, DW_TAG_compile_unit);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_producer);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_language);
    dwarf_uleb128(b, DW_FORM_data2);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_comp_dir);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_low_pc);
    dwarf_uleb128(b, DW_FORM_addr);
    dwarf_uleb128(b, DW_AT_high_pc);
    dwarf_uleb128(b, DW_FORM_data4);
    dwarf_uleb128(b, DW_AT_stmt_list);
    dwarf_uleb128(b, DW_FORM_data4);
    bytebuf_append(b, 0); /* end of attributes */
    bytebuf_append(b, 0);
    bytebuf_append(b, 0); /* end of abbrev table */
}

static void info_cu(ByteBuf *b, Vec *relocs, const char *compile_unit, const char *comp_dir,
                    CodegenModule *cm)
{
    bytebuf_append_u32(b, 0); /* unit length: patched in dwarf_build */
    bytebuf_append_u16(b, DW_INFO_VERSION);
    bytebuf_append_u32(b, 0); /* abbrev offset */
    bytebuf_append(b, DW_ADDRESS_SIZE);

    dwarf_uleb128(b, 1); /* abbrev code: compile_unit */
    size_t n = strlen("ficc");
    bytebuf_append_bytes(b, (const u8 *) "ficc", n);
    bytebuf_append(b, 0);
    bytebuf_append_u16(b, DW_LANG_C11);
    n = strlen(compile_unit);
    bytebuf_append_bytes(b, (const u8 *) compile_unit, n);
    bytebuf_append(b, 0);
    n = strlen(comp_dir);
    bytebuf_append_bytes(b, (const u8 *) comp_dir, n);
    bytebuf_append(b, 0);

    size_t text_size = 0;
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        text_size += bytebuf_len(((CodegenFunc *) vec_get(cm->funcs, i))->bytes);
    }

    /* low_pc = .text start (reloc vs .text); high_pc = length (DWARF4 size). */
    size_t slot = bytebuf_len(b);
    bytebuf_append_u64(b, 0);
    DwarfReloc *lo = arena_alloc(b->arena, sizeof(DwarfReloc), sizeof(void *));
    lo->offset = (u64) slot;
    lo->addend = 0;
    vec_push(relocs, lo);

    bytebuf_append_u32(b, (u32) text_size);

    bytebuf_append_u32(b, 0); /* stmt_list: the single .debug_line unit */

    bytebuf_append(b, 0); /* null DIE: terminates the compile_unit children */
}

void dwarf_build(CodegenModule *cm, const char *compile_unit, const char *comp_dir,
                 DwarfOutput *out, Arena *arena)
{
    bytebuf_init(&out->debug_info, arena);
    bytebuf_init(&out->debug_abbrev, arena);
    bytebuf_init(&out->debug_str, arena);
    bytebuf_init(&out->debug_line, arena);
    out->rela_info = vec_new(arena);
    out->rela_line = vec_new(arena);

    ByteBuf *b = &out->debug_line;
    size_t unit_length_off = bytebuf_len(b);
    line_header(b, compile_unit);
    size_t header_length_off = unit_length_off + sizeof(u32) + sizeof(u16);
    size_t header_end = bytebuf_len(b);

    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        line_func(b, out->rela_line, (CodegenFunc *) vec_get(cm->funcs, i));
    }
    size_t total = bytebuf_len(b);
    bytebuf_poke_u32(b, unit_length_off, (u32) (total - unit_length_off - sizeof(u32)));
    bytebuf_poke_u32(b, header_length_off, (u32) (header_end - header_length_off - sizeof(u32)));

    abbrev_emit(&out->debug_abbrev);

    ByteBuf *ib = &out->debug_info;
    size_t cu_length_off = bytebuf_len(ib);
    info_cu(ib, out->rela_info, compile_unit, comp_dir, cm);
    bytebuf_poke_u32(ib, cu_length_off, (u32) (bytebuf_len(ib) - cu_length_off - sizeof(u32)));
}