#ifndef FICC_TEST_DWARFCHECK_H
#define FICC_TEST_DWARFCHECK_H

#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

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
    DW_OP_reg0 = 0x50, /* reg0..reg31 = 0x50..0x6f */
    DW_OP_fbreg = 0x91,
    DW_OP_call_frame_cfa = 0x9c,
};

typedef struct
{
    u64 offset;
    i64 addend;
    u32 sym;
} DwarfCheckReloc;

typedef struct
{
    Arena *arena;
    const char *err;
    bool linked; /* ET_EXEC: address slots hold linked values, not reloc addends */

    /* section contents; NULL/0 when the section is absent */
    const u8 *debug_info;
    size_t debug_info_len;
    const u8 *debug_abbrev;
    size_t debug_abbrev_len;
    const u8 *debug_line;
    size_t debug_line_len;
    const u8 *debug_str;
    size_t debug_str_len;
    const u8 *debug_loc;
    size_t debug_loc_len;
    const u8 *eh_frame;
    size_t eh_frame_len;
    const u8 *text;
    size_t text_len;

    Vec *rela_info;
    Vec *rela_line;
    Vec *rela_eh;
} DwarfCheck;

void dwarf_check_load(const char *path, DwarfCheck *out, Arena *arena);

/* Reloc vectors are taken over, not copied. */
void dwarf_check_from_buffers(DwarfCheck *out, Arena *arena, const u8 *info, size_t info_len,
                              const u8 *abbrev, size_t abbrev_len, const u8 *line, size_t line_len,
                              const u8 *loc, size_t loc_len, const u8 *eh, size_t eh_len,
                              const u8 *text, size_t text_len, Vec *rela_info, Vec *rela_line,
                              Vec *rela_eh);

typedef struct
{
    u64 addr;
    i64 line;
    u32 file;
    bool end_seq;
} DwarfCheckRow;

typedef struct
{
    u64 slot;
    u64 value;
} DwarfCheckSetAddr;

typedef struct
{
    Vec *rows;
    Vec *set_addresses;
} DwarfCheckLines;

DwarfCheckLines *dwarf_check_lines(DwarfCheck *out, Arena *arena);

enum DWAttrKind
{
    DW_ATTR_NUM,
    DW_ATTR_REF,
    DW_ATTR_STR,
    DW_ATTR_LOC,
    DW_ATTR_ADDR,
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
    u32 value_off;
} DwarfCheckAttr;

typedef struct
{
    u32 off;
    u32 tag;
    bool has_children;
    Vec *attrs;
} DwarfCheckDie;

typedef struct
{
    u16 version;
    u32 abbrev_off;
    u8 addr_size;
    Arena *arena;
    Vec *dies; /* tree order, children follow parents */
    Vec *refs;
} DwarfCheckInfo;

DwarfCheckInfo *dwarf_check_info(DwarfCheck *out, Arena *arena);

typedef struct
{
    bool is_cie;
    u32 offset;
    u32 cie_fde_pointer;
    u32 initial_slot;
    u64 fde_begin;
    u64 fde_range;
} DwarfCheckEhEntry;

typedef struct
{
    Vec *entries; /* CIE first */
    Vec *cfa_ops;
    size_t nfde;
} DwarfCheckEh;

DwarfCheckEh *dwarf_check_eh(DwarfCheck *out, Arena *arena);

size_t dwarf_check_sleb128(const u8 *buf, size_t len, i64 *out);

DwarfCheckAttr *dwarf_check_attr(const DwarfCheckDie *die, u32 attr);

typedef struct
{
    u64 begin;
    u64 end;
    const u8 *expr;
    u32 expr_len;
} DwarfCheckLocRange;

Vec *dwarf_check_locs(DwarfCheck *out, u64 off, Arena *arena);

Vec *dwarf_check_dies_by_tag(const DwarfCheckInfo *info, u32 tag);

DwarfCheckDie *dwarf_check_die_named(const DwarfCheckInfo *info, const char *name);

bool dwarf_check_info_relocs_covered(DwarfCheck *out, const DwarfCheckInfo *info, u32 sym_lo,
                                     u32 sym_hi);

bool dwarf_check_line_relocs_covered(DwarfCheck *out, const DwarfCheckLines *lines, u32 sym_lo,
                                     u32 sym_hi);

#endif
