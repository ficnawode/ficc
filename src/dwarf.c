#include "dwarf.h"
#include "util/assert.h"
#include "util/hashmap.h"

#include <string.h>

/* .debug_line (DWARF2 §6.2). */

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
    DW_TAG_array_type = 0x01,
    DW_TAG_enumeration_type = 0x04,
    DW_TAG_formal_parameter = 0x05,
    DW_TAG_member = 0x0d,
    DW_TAG_pointer_type = 0x0f,
    DW_TAG_compile_unit = 0x11,
    DW_TAG_structure_type = 0x13,
    DW_TAG_subroutine_type = 0x15,
    DW_TAG_union_type = 0x17,
    DW_TAG_subrange_type = 0x21,
    DW_TAG_base_type = 0x24,
    DW_TAG_const_type = 0x26,
    DW_TAG_subprogram = 0x2e,
    DW_TAG_variable = 0x34,
};

enum DwarfAttr
{
    DW_AT_location = 0x02,
    DW_AT_name = 0x03,
    DW_AT_byte_size = 0x0b,
    DW_AT_bit_size = 0x0d,
    DW_AT_data_member_location = 0x38,
    DW_AT_bit_offset = 0x0c,
    DW_AT_stmt_list = 0x10,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_language = 0x13,
    DW_AT_comp_dir = 0x1b,
    DW_AT_producer = 0x25,
    DW_AT_count = 0x37,
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
    DW_FORM_udata = 0x0f,
    DW_FORM_exprloc = 0x18,
    DW_FORM_sec_offset = 0x17,
};

enum DwarfOp
{
    DW_OP_addr = 0x03,
    DW_OP_reg0 = 0x50, /* reg0..reg31 = 0x50..0x6f */
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
    ABBREV_POINTER_TYPE = 7,
    ABBREV_CONST_TYPE = 8,
    ABBREV_ARRAY_TYPE = 9,
    ABBREV_SUBRANGE_TYPE = 10,
    ABBREV_STRUCT_TYPE = 11,
    ABBREV_UNION_TYPE = 12,
    ABBREV_STRUCT_INCOMPLETE = 13,
    ABBREV_UNION_INCOMPLETE = 14,
    ABBREV_MEMBER = 15,
    ABBREV_MEMBER_BITFIELD = 16,
    ABBREV_ENUM_TYPE = 17,
    ABBREV_SUBROUTINE_TYPE = 18,
    ABBREV_SUBPROG_PARAM = 19,
    ABBREV_LOCAL_VARIABLE = 20,
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
            /* Decoder registers only move via emitted opcodes. */
            line_advance_pc(b, &addr, target);
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

/* .debug_info: one DWARF4 compile-unit tree. */

typedef struct
{
    size_t slot; /* byte position of a u32 ref4 field to backpatch */
    Type *type;
} PendingRef;

typedef struct
{
    ByteBuf *b;          /* .debug_info being built */
    Vec *relocs;         /* .rela.debug_info */
    ByteBuf *loc;        /* .debug_loc being built */
    U64Map *type_to_die; /* Type* -> DIE byte offset (0: not yet emitted) */
    u64 void_die;        /* cached fallback DIE for `void` references */
    Vec *pending;        /* Vec<PendingRef*>: type refs patched before the CU ends */
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

/* The CFA sits 16 bytes above %rbp: the saved rbp plus the return address. */
#define CFA_TO_RBP 16

/* DW_OP_fbreg plus the widest SLEB128 an i64 displacement can take. */
#define FBREG_EXPR_MAX 11

/* One `.debug_loc` range: [begin, end) carries `expr`.  DWARF4 defaults the
   base address to the CU's `DW_AT_low_pc`, so the entries are CU-relative. */
static void loc_range(InfoCtx *c, i64 begin, i64 end, const u8 *expr, u32 expr_len)
{
    bytebuf_append_u64(c->loc, (u64) begin);
    bytebuf_append_u64(c->loc, (u64) end);
    bytebuf_append_u16(c->loc, (u16) expr_len);
    bytebuf_append_bytes(c->loc, expr, expr_len);
}

/* Terminate a location list (DWARF4 §2.6.2). */
static void loc_list_end(InfoCtx *c)
{
    bytebuf_append_u64(c->loc, 0);
    bytebuf_append_u64(c->loc, 0);
}

/* Write expr = DW_OP_fbreg(disp) into `out` (>= FBREG_EXPR_MAX bytes). */
static u32 fbreg_expr(Arena *arena, u8 *out, i64 disp)
{
    ByteBuf tmp;
    bytebuf_init(&tmp, arena);
    dwarf_sleb128(&tmp, disp);
    out[0] = DW_OP_fbreg;
    memcpy(out + 1, bytebuf_data(&tmp), bytebuf_len(&tmp));
    return (u32) (1 + bytebuf_len(&tmp));
}

/* The DWARF register number a parameter lives in while it holds a physical reg. */
static u8 dwarf_reg_number(Type *type, int phys)
{
    if (type_is_fp(type))
    {
        return x86_dwarf_xmm_number((u8) phys);
    }
    return x86_dwarf_gpr_number((u8) phys);
}

/* A segment's inclusive position span as a function-relative byte range: a
   value defined at `start` is available past that instruction, and dies past
   its last use at `end`.  `entry_home` is set for a parameter, whose home is
   written by the prologue and so is valid from the body start (position 0).
   Returns false for an empty span. */
static bool segment_bounds(CodegenFunc *cf, const RegSegment *seg, bool entry_home, i64 *out_begin,
                           i64 *out_end)
{
    i64 func_off = (i64) cf->offset;
    i64 body = func_off + (i64) cf->frame.off_params;
    i64 fn_end = func_off + (i64) bytebuf_len(cf->bytes);
    const u32 *off = cf->position_offsets;
    i64 begin = (entry_home && seg->start == 0) ? body : func_off + (i64) off[seg->start];
    i64 end = func_off + (i64) off[seg->end];
    if (begin < body)
    {
        begin = body;
    }
    if (end > fn_end)
    {
        end = fn_end;
    }
    if (begin >= end)
    {
        return false;
    }
    *out_begin = begin;
    *out_end = end;
    return true;
}

/* Emit one location-list entry for a segment; returns its byte end (0 when empty). */
static i64 emit_segment_loc(InfoCtx *c, CodegenFunc *cf, u32 vreg, const RegSegment *seg,
                            Type *type, bool entry_home)
{
    i64 begin;
    i64 end;
    if (!segment_bounds(cf, seg, entry_home, &begin, &end))
    {
        return 0;
    }
    if (seg->kind == SEG_REG)
    {
        u8 rexpr = (u8) (DW_OP_reg0 + dwarf_reg_number(type, seg->reg));
        loc_range(c, begin, end, &rexpr, 1);
    }
    else if (seg->kind == SEG_MEM)
    {
        u8 fexpr[FBREG_EXPR_MAX];
        u32 flen = fbreg_expr(c->loc->arena, fexpr, -(i64) cf->alloc->slot_map[vreg] - CFA_TO_RBP);
        loc_range(c, begin, end, fexpr, flen);
    }
    else
    {
        u8 fexpr[FBREG_EXPR_MAX];
        u32 flen = fbreg_expr(c->loc->arena, fexpr, (i64) cf->alloc->remat_disp[vreg] - CFA_TO_RBP);
        loc_range(c, begin, end, fexpr, flen);
    }
    return end;
}

/* A parameter's location list: one entry per segment, then the entry-printed
   stage slot over the remainder of the function (the slot keeps the incoming
   value for the whole frame). */
static void emit_param_locs(InfoCtx *c, CodegenFunc *cf, IrParam *p, size_t pi)
{
    const RegAllocation *alloc = cf->alloc;
    i64 fn_end = (i64) cf->offset + (i64) bytebuf_len(cf->bytes);
    i64 reached = (i64) cf->offset + (i64) cf->frame.off_params;
    for (u32 s = alloc->seg_begin[p->vreg]; s < alloc->seg_begin[p->vreg + 1]; s++)
    {
        i64 end = emit_segment_loc(c, cf, p->vreg, &alloc->segments[s], p->type, true);
        if (end > reached)
        {
            reached = end;
        }
    }
    if (cf->param_stage[pi] != 0 && reached < fn_end)
    {
        u8 fexpr[FBREG_EXPR_MAX];
        u32 flen = fbreg_expr(c->loc->arena, fexpr, -(i64) cf->param_stage[pi] - CFA_TO_RBP);
        loc_range(c, reached, fn_end, fexpr, flen);
    }
    loc_list_end(c);
}

/* True when any of the local's SSA versions has a non-empty segment. */
static bool local_has_loc(CodegenFunc *cf, IrLocal *l)
{
    const RegAllocation *alloc = cf->alloc;
    size_t nv = vec_size(l->vregs);
    for (size_t i = 0; i < nv; i++)
    {
        u32 vreg = *(u32 *) vec_get(l->vregs, i);
        if (vreg >= alloc->nvregs)
        {
            continue;
        }
        for (u32 s = alloc->seg_begin[vreg]; s < alloc->seg_begin[vreg + 1]; s++)
        {
            i64 begin;
            i64 end;
            if (segment_bounds(cf, &alloc->segments[s], false, &begin, &end))
            {
                return true;
            }
        }
    }
    return false;
}

/* A local's location list is the union of its SSA versions' segment entries. */
static void emit_local_locs(InfoCtx *c, CodegenFunc *cf, IrLocal *l)
{
    const RegAllocation *alloc = cf->alloc;
    size_t nv = vec_size(l->vregs);
    for (size_t i = 0; i < nv; i++)
    {
        u32 vreg = *(u32 *) vec_get(l->vregs, i);
        if (vreg >= alloc->nvregs)
        {
            continue;
        }
        for (u32 s = alloc->seg_begin[vreg]; s < alloc->seg_begin[vreg + 1]; s++)
        {
            emit_segment_loc(c, cf, vreg, &alloc->segments[s], l->type, false);
        }
    }
    loc_list_end(c);
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

/* Compound type DIEs: pointer/const/array/record/enum/func. */

static u64 type_die(InfoCtx *c, Type *t);

/* Claim the CU offset up front so self-referential records terminate. */
static u32 type_die_reserve(InfoCtx *c, Type *t)
{
    u32 off = (u32) bytebuf_len(c->b);
    u64map_set(c->type_to_die, (u64) (uintptr_t) t, (void *) (uintptr_t) off);
    return off;
}

static u32 type_die_lookup(InfoCtx *c, Type *t)
{
    return (u32) (uintptr_t) u64map_get(c->type_to_die, (u64) (uintptr_t) t);
}

/* Backpatch slot with t's offset; unresolved types defer so no DIE nests. */
static void type_poke(InfoCtx *c, size_t slot, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        bytebuf_poke_u32(c->b, slot, off);
        return;
    }
    PendingRef *p = arena_alloc(c->b->arena, sizeof(PendingRef), sizeof(void *));
    p->slot = slot;
    p->type = t;
    vec_push(c->pending, p);
}

/* Emit a u32 ref4 placeholder for `t`; resolves via type_poke. */
static size_t type_ref_emit(InfoCtx *c, Type *t)
{
    size_t slot = bytebuf_len(c->b);
    bytebuf_append_u32(c->b, 0);
    type_poke(c, slot, t);
    return slot;
}

static u64 pointer_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    /* Pointee DIE first; member cycles resolve through the pending list. */
    u64 pointee = type_die(c, t->ptr.pointee);
    off = (u32) bytebuf_len(c->b);
    u64map_set(c->type_to_die, (u64) (uintptr_t) t, (void *) (uintptr_t) off);
    dwarf_uleb128(c->b, ABBREV_POINTER_TYPE);
    bytebuf_append_u32(c->b, (u32) pointee);
    return off;
}

static u64 const_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    u64 base = type_die(c, type_unqual(t));
    off = (u32) bytebuf_len(c->b);
    u64map_set(c->type_to_die, (u64) (uintptr_t) t, (void *) (uintptr_t) off);
    dwarf_uleb128(c->b, ABBREV_CONST_TYPE);
    bytebuf_append_u32(c->b, (u32) base);
    return off;
}

static u64 array_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    u64 elem = type_die(c, t->arr.elem);
    u64 index = type_die(c, type_ulong()); /* subrange index type (size_t) */
    off = type_die_reserve(c, t);
    dwarf_uleb128(c->b, ABBREV_ARRAY_TYPE);
    bytebuf_append_u32(c->b, (u32) elem);
    dwarf_uleb128(c->b, ABBREV_SUBRANGE_TYPE);
    bytebuf_append_u32(c->b, (u32) index);
    dwarf_uleb128(c->b, t->arr.length);
    bytebuf_append(c->b, 0); /* end of array_type's children */
    return off;
}

static u64 enum_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    off = type_die_reserve(c, t);
    dwarf_uleb128(c->b, ABBREV_ENUM_TYPE);
    info_string(c, t->enumm.tag ? t->enumm.tag : "");
    /* Constants are folded away, so there are no enumerator children. */
    dwarf_uleb128(c->b, t->size);
    return off;
}

static u64 record_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    off = type_die_reserve(c, t);
    bool is_struct = t->kind == TYPE_STRUCT;
    if (!t->record.complete)
    {
        /* Forward-declared `struct S;`: name only, no byte_size, no members. */
        dwarf_uleb128(c->b, is_struct ? ABBREV_STRUCT_INCOMPLETE : ABBREV_UNION_INCOMPLETE);
        info_string(c, t->record.tag ? t->record.tag : "");
        return off;
    }
    dwarf_uleb128(c->b, is_struct ? ABBREV_STRUCT_TYPE : ABBREV_UNION_TYPE);
    info_string(c, t->record.tag ? t->record.tag : "");
    dwarf_uleb128(c->b, t->size);

    Vec *fields = t->record.fields;
    size_t nfields = vec_size(fields);
    for (size_t i = 0; i < nfields; i++)
    {
        RecordField *f = (RecordField *) vec_get(fields, i);
        dwarf_uleb128(c->b, f->bit_width >= 0 ? ABBREV_MEMBER_BITFIELD : ABBREV_MEMBER);
        info_string(c, f->name ? f->name : "");
        type_ref_emit(c, f->type);
        dwarf_uleb128(c->b, f->offset);
        if (f->bit_width >= 0)
        {
            /* Bit offset counts from the storage unit's MSb (little-endian). */
            i64 unit_bits = (i64) f->type->size * 8;
            dwarf_uleb128(c->b, (u64) f->bit_width);
            dwarf_uleb128(c->b, (u64) (unit_bits - f->bit_offset - f->bit_width));
        }
    }
    bytebuf_append(c->b, 0); /* end of the record's children */
    return off;
}

static u64 func_die(InfoCtx *c, Type *t)
{
    u32 off = type_die_lookup(c, t);
    if (off)
    {
        return off;
    }
    /* Reserve first: `int (*f)(int (*f)(int))` self-references recurse. */
    off = type_die_reserve(c, t);
    dwarf_uleb128(c->b, ABBREV_SUBROUTINE_TYPE);
    type_ref_emit(c, t->func.ret);
    size_t nparams = vec_size(t->func.params);
    for (size_t i = 0; i < nparams; i++)
    {
        dwarf_uleb128(c->b, ABBREV_SUBPROG_PARAM);
        type_ref_emit(c, (Type *) vec_get(t->func.params, i));
    }
    bytebuf_append(c->b, 0); /* end of subroutine_type's children */
    return off;
}

/* Fundamental scalar -> its base-type DIE; compound types get their own DIEs. */
static u64 type_die(InfoCtx *c, Type *t)
{
    if (!t)
    {
        return void_base_type(c);
    }
    if (t->kind == TYPE_ARRAY)
    {
        return array_die(c, t);
    }
    if (t->qualifiers & Q_CONST)
    {
        return const_die(c, t);
    }
    u32 off = type_die_lookup(c, t);
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
        case TYPE_PTR:
            return pointer_die(c, t);
        case TYPE_STRUCT:
        case TYPE_UNION:
            return record_die(c, t);
        case TYPE_ENUM:
            return enum_die(c, t);
        case TYPE_FUNC:
            return func_die(c, t);
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
    type_ref_emit(c, cf->func->ret_type);

    size_t nparams = vec_size(cf->func->params);
    for (size_t i = 0; i < nparams; i++)
    {
        IrParam *p = (IrParam *) vec_get(cf->func->params, i);
        dwarf_uleb128(c->b, ABBREV_FORMAL_PARAM);
        info_string(c, p->name);
        type_ref_emit(c, p->type);
        u32 loc_off = (u32) bytebuf_len(c->loc);
        emit_param_locs(c, cf, p, i);
        bytebuf_append_u32(c->b, loc_off);
    }
    size_t nlocals = vec_size(cf->func->locals);
    for (size_t i = 0; i < nlocals; i++)
    {
        IrLocal *l = (IrLocal *) vec_get(cf->func->locals, i);
        if (vec_size(l->vregs) == 0 || !local_has_loc(cf, l))
        {
            continue;
        }
        dwarf_uleb128(c->b, ABBREV_LOCAL_VARIABLE);
        info_string(c, l->name);
        type_ref_emit(c, l->type);
        u32 loc_off = (u32) bytebuf_len(c->loc);
        emit_local_locs(c, cf, l);
        bytebuf_append_u32(c->b, loc_off);
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
    type_ref_emit(c, g->type);
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
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
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
    dwarf_uleb128(b, DW_FORM_sec_offset);
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

    /* Rich type abbreviations. */

    dwarf_uleb128(b, ABBREV_POINTER_TYPE);
    dwarf_uleb128(b, DW_TAG_pointer_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_CONST_TYPE);
    dwarf_uleb128(b, DW_TAG_const_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_ARRAY_TYPE);
    dwarf_uleb128(b, DW_TAG_array_type);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_SUBRANGE_TYPE);
    dwarf_uleb128(b, DW_TAG_subrange_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_count);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_STRUCT_TYPE);
    dwarf_uleb128(b, DW_TAG_structure_type);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_byte_size);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_UNION_TYPE);
    dwarf_uleb128(b, DW_TAG_union_type);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_byte_size);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_STRUCT_INCOMPLETE);
    dwarf_uleb128(b, DW_TAG_structure_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_UNION_INCOMPLETE);
    dwarf_uleb128(b, DW_TAG_union_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_MEMBER);
    dwarf_uleb128(b, DW_TAG_member);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_data_member_location);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_MEMBER_BITFIELD);
    dwarf_uleb128(b, DW_TAG_member);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_data_member_location);
    dwarf_uleb128(b, DW_FORM_udata);
    dwarf_uleb128(b, DW_AT_bit_size);
    dwarf_uleb128(b, DW_FORM_udata);
    dwarf_uleb128(b, DW_AT_bit_offset);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_ENUM_TYPE);
    dwarf_uleb128(b, DW_TAG_enumeration_type);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_byte_size);
    dwarf_uleb128(b, DW_FORM_udata);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_SUBROUTINE_TYPE);
    dwarf_uleb128(b, DW_TAG_subroutine_type);
    dwarf_uleb128(b, DW_CHILDREN_YES);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_SUBPROG_PARAM);
    dwarf_uleb128(b, DW_TAG_formal_parameter);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    bytebuf_append(b, 0);
    bytebuf_append(b, 0);

    dwarf_uleb128(b, ABBREV_LOCAL_VARIABLE);
    dwarf_uleb128(b, DW_TAG_variable);
    dwarf_uleb128(b, 0);
    dwarf_uleb128(b, DW_AT_name);
    dwarf_uleb128(b, DW_FORM_string);
    dwarf_uleb128(b, DW_AT_type);
    dwarf_uleb128(b, DW_FORM_ref4);
    dwarf_uleb128(b, DW_AT_location);
    dwarf_uleb128(b, DW_FORM_sec_offset);
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
    bytebuf_init(&out->debug_loc, arena);
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
        .loc = &out->debug_loc,
        .type_to_die = u64map_new(arena),
        .void_die = 0,
        .pending = vec_new(arena),
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
        ByteBuf scratch_ro, scratch_data, scratch_init, scratch_fini;
        bytebuf_init(&scratch_ro, arena);
        bytebuf_init(&scratch_data, arena);
        bytebuf_init(&scratch_init, arena);
        bytebuf_init(&scratch_fini, arena);
        u64 *global_off = codegen_global_offsets(cm, &scratch_ro, &scratch_data, &scratch_init,
                                                 &scratch_fini, arena);
        for (size_t i = 0; i < nglobals; i++)
        {
            global_emit(&c, (IrGlobal *) vec_get(cm->globals, i), global_off[i]);
        }
    }

    /* Flush deferred type DIEs; a record resolved here queues its own members. */
    for (size_t i = 0; i < vec_size(c.pending); i++)
    {
        PendingRef *p = (PendingRef *) vec_get(c.pending, i);
        bytebuf_poke_u32(c.b, p->slot, (u32) type_die(&c, p->type));
    }

    bytebuf_append(c.b, 0); /* null DIE: terminates the compile_unit children */
    bytebuf_poke_u32(c.b, cu_length_off, (u32) (bytebuf_len(c.b) - cu_length_off - sizeof(u32)));
}