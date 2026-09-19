#include "dwarf.h"
#include "util/assert.h"
#include "util/hashmap.h"

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
    DW_TAG_formal_parameter = 0x05,
    DW_TAG_compile_unit = 0x11,
    DW_TAG_base_type = 0x24,
    DW_TAG_subprogram = 0x2e,
    DW_TAG_variable = 0x34,
};

enum DwarfAttr
{
    DW_AT_location = 0x02,
    DW_AT_name = 0x03,
    DW_AT_byte_size = 0x0b,
    DW_AT_stmt_list = 0x10,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_language = 0x13,
    DW_AT_comp_dir = 0x1b,
    DW_AT_producer = 0x25,
    DW_AT_encoding = 0x3e,
    DW_AT_external = 0x3f,
    DW_AT_frame_base = 0x40,
    DW_AT_type = 0x49,
};

enum DwarfForm
{
    DW_FORM_addr = 0x01,
    DW_FORM_data1 = 0x0b,
    DW_FORM_data2 = 0x05,
    DW_FORM_data4 = 0x06,
    DW_FORM_string = 0x08,
    DW_FORM_flag = 0x0c,
    DW_FORM_ref4 = 0x13,
    DW_FORM_exprloc = 0x18,
};

enum DwarfOp
{
    DW_OP_addr = 0x03,
    DW_OP_fbreg = 0x91,
    DW_OP_call_frame_cfa = 0x9c,
};

enum DwarfAtE
{
    DW_ATE_address = 0x01,
    DW_ATE_boolean = 0x02,
    DW_ATE_float = 0x04,
    DW_ATE_signed = 0x05,
    DW_ATE_signed_char = 0x06,
    DW_ATE_unsigned = 0x07,
    DW_ATE_unsigned_char = 0x08,
};

/* Abbrev codes: the fixed table emitted into .debug_abbrev in this order. */
enum
{
    ABBREV_CU = 1,
    ABBREV_SUBPROGRAM = 2,
    ABBREV_FORMAL_PARAM = 3,
    ABBREV_VARIABLE = 4,
    ABBREV_BASE_TYPE = 5,
    ABBREV_VARIABLE_NO_LOC = 6, /* externs declare no DW_AT_location */
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
    rel->sym = DWARF_SYM_TEXT;
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

/* ---- .debug_info: one DWARF4 compile-unit tree ---- */

typedef struct
{
    ByteBuf *b;          /* .debug_info being built */
    Vec *relocs;         /* .rela.debug_info */
    U64Map *type_to_die; /* Type* -> DIE byte offset (0: not yet emitted) */
    u64 void_die;        /* cached fallback DIE for types beyond this phase */
} InfoCtx;

static void info_reloc(InfoCtx *c, u32 sym, i64 addend)
{
    size_t slot = bytebuf_len(c->b);
    bytebuf_append_u64(c->b, 0);
    DwarfReloc *rel = arena_alloc(c->b->arena, sizeof(DwarfReloc), sizeof(void *));
    rel->offset = (u64) slot;
    rel->addend = addend;
    rel->sym = sym;
    vec_push(c->relocs, rel);
}

static void info_string(InfoCtx *c, const char *s)
{
    size_t n = strlen(s);
    bytebuf_append_bytes(c->b, (const u8 *) s, n);
    bytebuf_append(c->b, 0);
}

/* exprloc = DW_OP_fbreg(disp): a fixed slot below the call-frame CFA. */
static void info_fbreg_loc(InfoCtx *c, i64 disp)
{
    ByteBuf tmp;
    bytebuf_init(&tmp, c->b->arena);
    dwarf_sleb128(&tmp, disp);
    bytebuf_append(c->b, (u8) (1u + (u32) bytebuf_len(&tmp)));
    bytebuf_append(c->b, DW_OP_fbreg);
    bytebuf_append_bytes(c->b, bytebuf_data(&tmp), bytebuf_len(&tmp));
}

/* exprloc = DW_OP_addr: an R_X86_64_64 slot resolved to sym + addend. */
static void info_addr_loc(InfoCtx *c, u32 sym, i64 addend)
{
    bytebuf_append(c->b, (u8) (1u + DW_ADDRESS_SIZE));
    bytebuf_append(c->b, DW_OP_addr);
    size_t slot = bytebuf_len(c->b);
    bytebuf_append_u64(c->b, 0);
    DwarfReloc *rel = arena_alloc(c->b->arena, sizeof(DwarfReloc), sizeof(void *));
    rel->offset = (u64) slot;
    rel->addend = addend;
    rel->sym = sym;
    vec_push(c->relocs, rel);
}

static u64 dump_base_type(InfoCtx *c, const char *name, u8 byte_size, u8 encoding)
{
    u64 die = bytebuf_len(c->b);
    dwarf_uleb128(c->b, ABBREV_BASE_TYPE);
    info_string(c, name);
    bytebuf_append(c->b, byte_size);
    bytebuf_append(c->b, encoding);
    return die;
}

/* Fallback void DIE for types this phase does not describe. */
static u64 void_base_type(InfoCtx *c)
{
    if (c->void_die == 0)
    {
        c->void_die = dump_base_type(c, "void", 0, DW_ATE_address);
    }
    return c->void_die;
}

/* Fundamental scalar -> its base-type DIE; everything else -> void fallback. */
static u64 type_die(InfoCtx *c, Type *t)
{
    t = type_unqual(t);
    if (!t)
    {
        return void_base_type(c);
    }
    u32 off = (u32) (uintptr_t) u64map_get(c->type_to_die, (u64) (uintptr_t) t);
    if (off)
    {
        return off;
    }
    const char *name;
    u8 size;
    u8 encoding;
    switch (t->kind)
    {
        case TYPE_VOID:
            return void_base_type(c);
        case TYPE_BOOL:
            name = "_Bool";
            size = (u8) (t->size);
            encoding = DW_ATE_boolean;
            break;
        case TYPE_CHAR:
            name = "char";
            size = (u8) (t->size);
            encoding = DW_ATE_signed_char;
            break;
        case TYPE_SHORT:
            name = "short";
            size = (u8) (t->size);
            encoding = DW_ATE_signed;
            break;
        case TYPE_INT:
            name = "int";
            size = (u8) (t->size);
            encoding = DW_ATE_signed;
            break;
        case TYPE_LONG:
            name = "long";
            size = (u8) (t->size);
            encoding = DW_ATE_signed;
            break;
        case TYPE_LLONG:
            name = "long long";
            size = (u8) (t->size);
            encoding = DW_ATE_signed;
            break;
        case TYPE_UCHAR:
            name = "unsigned char";
            size = (u8) (t->size);
            encoding = DW_ATE_unsigned_char;
            break;
        case TYPE_USHORT:
            name = "unsigned short";
            size = (u8) (t->size);
            encoding = DW_ATE_unsigned;
            break;
        case TYPE_UINT:
            name = "unsigned int";
            size = (u8) (t->size);
            encoding = DW_ATE_unsigned;
            break;
        case TYPE_ULONG:
            name = "unsigned long";
            size = (u8) (t->size);
            encoding = DW_ATE_unsigned;
            break;
        case TYPE_ULLONG:
            name = "unsigned long long";
            size = (u8) (t->size);
            encoding = DW_ATE_unsigned;
            break;
        case TYPE_FLOAT:
            name = "float";
            size = (u8) (t->size);
            encoding = DW_ATE_float;
            break;
        case TYPE_DOUBLE:
            name = "double";
            size = (u8) (t->size);
            encoding = DW_ATE_float;
            break;
        case TYPE_LONG_DOUBLE:
            name = "long double";
            size = (u8) (t->size);
            encoding = DW_ATE_float;
            break;
        default:
            return void_base_type(c);
    }
    off = (u32) dump_base_type(c, name, size, encoding);
    u64map_set(c->type_to_die, (u64) (uintptr_t) t, (void *) (uintptr_t) off);
    return off;
}

/* Pre-emit referenced base types so ref4 offsets point at complete sibling DIEs. */
static void preemit_types(InfoCtx *c, CodegenModule *cm)
{
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        size_t nparams = vec_size(cf->func->params);
        for (size_t j = 0; j < nparams; j++)
        {
            type_die(c, ((IrParam *) vec_get(cf->func->params, j))->type);
        }
    }
    size_t nglobals = cm->globals ? vec_size(cm->globals) : 0;
    for (size_t i = 0; i < nglobals; i++)
    {
        type_die(c, ((IrGlobal *) vec_get(cm->globals, i))->type);
    }
}

static void subprogram_emit(InfoCtx *c, CodegenFunc *cf)
{
    dwarf_uleb128(c->b, ABBREV_SUBPROGRAM);
    bytebuf_append(c->b, (u8) (cf->is_static ? 0 : 1));
    info_string(c, cf->name);
    info_reloc(c, DWARF_SYM_TEXT, (i64) cf->offset);
    bytebuf_append_u32(c->b, (u32) bytebuf_len(cf->bytes)); /* high_pc = size */
    bytebuf_append(c->b, 1);                                /* frame_base exprloc length */
    bytebuf_append(c->b, DW_OP_call_frame_cfa);

    size_t nparams = vec_size(cf->func->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(cf->func->params, i);
        dwarf_uleb128(c->b, ABBREV_FORMAL_PARAM);
        info_string(c, p->name);
        bytebuf_append_u32(c->b, (u32) type_die(c, p->type));
        /* Slots sit below %rbp; the CFA is 16 bytes above it (push rbp + ret). */
        info_fbreg_loc(c, -(i64) cf->slot_off[p->vreg] - 16);
    }
    bytebuf_append(c->b, 0); /* end of this subprogram's children */
}

static u32 global_section_sym(IrGlobal *g)
{
    switch (g->section)
    {
        case IR_SECTION_RODATA:
            return DWARF_SYM_RODATA;
        case IR_SECTION_DATA:
            return DWARF_SYM_DATA;
        default:
            return DWARF_SYM_BSS;
    }
}

static void global_emit(InfoCtx *c, IrGlobal *g, u64 offset)
{
    bool has_loc = g->linkage != IR_LINK_EXTERN;
    dwarf_uleb128(c->b, has_loc ? ABBREV_VARIABLE : ABBREV_VARIABLE_NO_LOC);
    info_string(c, g->name);
    bytebuf_append(c->b, (u8) (g->linkage == IR_LINK_LOCAL ? 0 : 1));
    bytebuf_append_u32(c->b, (u32) type_die(c, g->type));
    if (has_loc)
    {
        info_addr_loc(c, global_section_sym(g), (i64) offset);
    }
}

static void abbrev_emit(ByteBuf *b)
{
    dwarf_uleb128(b, ABBREV_CU);
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
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_SUBPROGRAM);
    dwarf_uleb128(b, DW_TAG_subprogram);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_external);
    dwarf_uleb128(b, DW_FORM_flag);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_low_pc);
    dwarf_uleb128(b, DW_FORM_addr);
    dwarf_uleb128(b, DW_AT_high_pc);
    dwarf_uleb128(b, DW_FORM_data4);
    dwarf_uleb128(b, DW_AT_frame_base);
    dwarf_uleb128(b, DW_FORM_exprloc);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_FORMAL_PARAM);
    dwarf_uleb128(b, DW_TAG_formal_parameter);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_location);
    dwarf_uleb128(b, DW_FORM_exprloc);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_VARIABLE);
    dwarf_uleb128(b, DW_TAG_variable);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_external);
    dwarf_uleb128(b, DW_FORM_flag);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_location);
    dwarf_uleb128(b, DW_FORM_exprloc);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_VARIABLE_NO_LOC);
    dwarf_uleb128(b, DW_TAG_variable);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_external);
    dwarf_uleb128(b, DW_FORM_flag);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_BASE_TYPE);
    dwarf_uleb128(b, DW_TAG_base_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_byte_size);
    dwarf_uleb128(b, DW_FORM_data1);
    dwarf_uleb128(b, DW_AT_encoding);
    dwarf_uleb128(b, DW_FORM_data1);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    bytebuf_append(b, 0); /* end of the abbrev table */
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

    InfoCtx c = {
        .b = &out->debug_info,
        .relocs = out->rela_info,
        .type_to_die = u64map_new(arena),
        .void_die = 0,
    };

    size_t cu_length_off = bytebuf_len(c.b);
    bytebuf_append_u32(c.b, 0); /* unit length: patched below */
    bytebuf_append_u16(c.b, DW_INFO_VERSION);
    bytebuf_append_u32(c.b, 0); /* abbrev offset: the table starts at 0 */
    bytebuf_append(c.b, DW_ADDRESS_SIZE);

    dwarf_uleb128(c.b, ABBREV_CU);
    info_string(&c, "ficc");
    bytebuf_append_u16(c.b, DW_LANG_C11);
    info_string(&c, compile_unit);
    info_string(&c, comp_dir);
    info_reloc(&c, DWARF_SYM_TEXT, 0); /* low_pc = .text start */

    size_t text_size = 0;
    for (size_t i = 0; i < nfuncs; i++)
    {
        text_size += bytebuf_len(((CodegenFunc *) vec_get(cm->funcs, i))->bytes);
    }
    bytebuf_append_u32(c.b, (u32) text_size); /* high_pc of the whole unit */
    bytebuf_append_u32(c.b, 0);               /* stmt_list: the one .debug_line unit */

    preemit_types(&c, cm);

    for (size_t i = 0; i < nfuncs; i++)
    {
        subprogram_emit(&c, (CodegenFunc *) vec_get(cm->funcs, i));
    }

    size_t nglobals = cm->globals ? vec_size(cm->globals) : 0;
    if (nglobals)
    {
        ByteBuf scratch_ro, scratch_data;
        bytebuf_init(&scratch_ro, arena);
        bytebuf_init(&scratch_data, arena);
        u64 *global_off = codegen_global_offsets(cm, &scratch_ro, &scratch_data, arena);
        for (size_t i = 0; i < nglobals; i++)
        {
            global_emit(&c, (IrGlobal *) vec_get(cm->globals, i), global_off[i]);
        }
    }

    bytebuf_append(c.b, 0); /* null DIE: terminates the compile_unit children */
    bytebuf_poke_u32(c.b, cu_length_off, (u32) (bytebuf_len(c.b) - cu_length_off - sizeof(u32)));
}