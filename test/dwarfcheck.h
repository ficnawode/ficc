#ifndef FICC_TEST_DWARFCHECK_H
#define FICC_TEST_DWARFCHECK_H

#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

/* In-process structural DWARF verifier for the ficc test suite. */

/* DWARF4 tag/attribute/opcode values (mirror src/dwarf.c & src/cfi.c). */
enum
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

enum
{
    DW_AT_location = 0x02,
    DW_AT_name = 0x03,
    DW_AT_byte_size = 0x0b,
    DW_AT_bit_offset = 0x0c,
    DW_AT_bit_size = 0x0d,
    DW_AT_low_pc = 0x11,
    DW_AT_high_pc = 0x12,
    DW_AT_count = 0x37,
    DW_AT_data_member_location = 0x38,
    DW_AT_external = 0x3f,
    DW_AT_frame_base = 0x40,
    DW_AT_type = 0x49,
};

enum
{
    DW_OP_addr = 0x03,
    DW_OP_fbreg = 0x91,
    DW_OP_call_frame_cfa = 0x9c,
};

typedef struct
{
    u64 offset; /* byte position of an 8-byte address slot inside its section */
    i64 addend; /* relocation addend */
    u32 sym;    /* symtab index of the section symbol the slot points at */
} DwarfCheckReloc;

typedef struct
{
    Arena *arena;
    const char *err; /* set on parse failure; NULL while clean */

    /* section contents; NULL/0 when the section is absent */
    const u8 *debug_info;
    size_t debug_info_len;
    const u8 *debug_abbrev;
    size_t debug_abbrev_len;
    const u8 *debug_line;
    size_t debug_line_len;
    const u8 *debug_str;
    size_t debug_str_len;
    const u8 *eh_frame;
    size_t eh_frame_len;
    const u8 *text;
    size_t text_len;

    /* parsed .rela.debug_info / .rela.debug_line / .rela.eh_frame lists */
    Vec *rela_info; /* Vec<DwarfCheckReloc*> */
    Vec *rela_line;
    Vec *rela_eh;
} DwarfCheck;

/* Fill from a ficc-produced ET_REL object file (missing sections stay NULL). */
void dwarf_check_load(const char *path, DwarfCheck *out, Arena *arena);

/* Fill from raw section buffers; the reloc vectors are taken over, not copied. */
void dwarf_check_from_buffers(DwarfCheck *out, Arena *arena, const u8 *info, size_t info_len,
                              const u8 *abbrev, size_t abbrev_len, const u8 *line, size_t line_len,
                              const u8 *eh, size_t eh_len, const u8 *text, size_t text_len,
                              Vec *rela_info, Vec *rela_line, Vec *rela_eh);

/* ---- .debug_line ---- */

typedef struct
{
    u64 addr; /* row address; functions sit at their .text offset */
    i64 line; /* source line (end-of-sequence rows keep the last line) */
    u32 file; /* file index, 1-based */
    bool end_seq;
} DwarfCheckRow;

typedef struct
{
    u64 slot;  /* byte position of the 8-byte address field in .debug_line */
    u64 value; /* the resolved address (a function's .text offset) */
} DwarfCheckSetAddr;

typedef struct
{
    Vec *rows;          /* Vec<DwarfCheckRow*> in program order */
    Vec *set_addresses; /* Vec<DwarfCheckSetAddr*>, in program order */
} DwarfCheckLines;

/* Decode the single-CU line program; NULL (with out->err) on malformed input. */
DwarfCheckLines *dwarf_check_lines(DwarfCheck *out, Arena *arena);

/* ---- .debug_info / .debug_abbrev ---- */

enum DWAttrKind
{
    DW_ATTR_NUM,  /* data1/data2/data4/udata/flag; value in `num` */
    DW_ATTR_REF,  /* ref4; target DIE offset in `ref` */
    DW_ATTR_STR,  /* DW_FORM_string; NUL-terminated, points into the info buf */
    DW_ATTR_LOC,  /* exprloc; `loc`/`loc_len`, `value_off` = pos of the length byte */
    DW_ATTR_ADDR, /* DW_FORM_addr; `addr` (le64), `value_off` = pos of the 8 bytes */
};

typedef struct
{
    u32 attr;
    int kind;
    u64 num;
    u32 ref;
    const char *str;
    const u8 *loc;
    u32 loc_len;
    u64 addr;
    u32 value_off; /* absolute .debug_info position of the form value */
} DwarfCheckAttr;

typedef struct
{
    u32 off; /* DIE offset within .debug_info */
    u32 tag;
    bool has_children;
    Vec *attrs; /* Vec<DwarfCheckAttr*> */
} DwarfCheckDie;

typedef struct
{
    u16 version;
    u32 abbrev_off;
    u8 addr_size;
    Arena *arena;
    Vec *dies; /* Vec<DwarfCheckDie*>, tree order, children follow parents */
    Vec *refs; /* Vec<u64*>: every ref4 attribute value seen */
} DwarfCheckInfo;

/* Walk the first CU's DIE tree and resolve every ref4; NULL on malformed input. */
DwarfCheckInfo *dwarf_check_info(DwarfCheck *out, Arena *arena);

/* ---- .eh_frame ---- */

typedef struct
{
    bool is_cie;
    u32 offset;          /* byte offset of the entry's length field */
    u32 cie_fde_pointer; /* CIE: 0; FDE: back-distance to its CIE */
    u32 initial_slot;    /* FDE: pos of the 8-byte initial_location field */
    u64 fde_begin;       /* FDE: resolved start address (reloc addend) */
    u64 fde_range;       /* FDE: address_range */
} DwarfCheckEhEntry;

typedef struct
{
    Vec *entries; /* Vec<DwarfCheckEhEntry*>, CIE first */
    Vec *cfa_ops; /* Vec<u64*>, one opcode value per decoded CFA instruction */
    size_t nfde;
} DwarfCheckEh;

/* Parse .eh_frame (CIE + one FDE per function); NULL on wrong shape. */
DwarfCheckEh *dwarf_check_eh(DwarfCheck *out, Arena *arena);

/* ---- helpers shared by the tests ---- */

/* Decode a signed LEB128; returns bytes consumed (0 when truncated). */
size_t dwarf_check_sleb128(const u8 *buf, size_t len, i64 *out);

/* One attr of a DIE, or NULL. */
DwarfCheckAttr *dwarf_check_attr(const DwarfCheckDie *die, u32 attr);

/* All DIEs with `tag`, in tree order (empty when none). */
Vec *dwarf_check_dies_by_tag(const DwarfCheckInfo *info, u32 tag);

/* The single DIE whose name string equals `name`, or NULL. */
DwarfCheckDie *dwarf_check_die_named(const DwarfCheckInfo *info, const char *name);

/* Every address slot (DW_FORM_addr, DW_OP_addr) has one reloc with sym in range. */
bool dwarf_check_info_relocs_covered(DwarfCheck *out, const DwarfCheckInfo *info, u32 sym_lo,
                                     u32 sym_hi);

/* Every set_address slot has one reloc with sym in range and a valid .text offset. */
bool dwarf_check_line_relocs_covered(DwarfCheck *out, const DwarfCheckLines *lines, u32 sym_lo,
                                     u32 sym_hi);

#endif