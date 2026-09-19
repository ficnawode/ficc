#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "dwarf.h"
#include "ir_builder.h"

static void expect_bytes(ByteBuf *b, const u8 *want, size_t n)
{
    EXPECT_EQ(bytebuf_len(b), n);
    if (bytebuf_len(b) == n)
    {
        EXPECT_EQ(memcmp(bytebuf_data(b), want, n), 0);
    }
}

static void lebs(ByteBuf *b, Arena *a, u64 val, const u8 *want, size_t n)
{
    bytebuf_init(b, a);
    dwarf_uleb128(b, val);
    expect_bytes(b, want, n);
}

TEST(dwarf_leb, uleb128_known_encodings)
{
    Arena *a = arena_new();
    ByteBuf b;

    lebs(&b, a, 0, (const u8[]) {0x00}, 1);
    lebs(&b, a, 127, (const u8[]) {0x7f}, 1);
    lebs(&b, a, 128, (const u8[]) {0x80, 0x01}, 2);
    lebs(&b, a, 624485, (const u8[]) {0xe5, 0x8e, 0x26}, 3);
    lebs(&b, a, 0xffffffffu, (const u8[]) {0xff, 0xff, 0xff, 0xff, 0x0f}, 5);

    arena_free(a);
}

TEST(dwarf_leb, sleb128_known_encodings)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, 0);
    expect_bytes(&b, (const u8[]) {0x00}, 1);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, 1);
    expect_bytes(&b, (const u8[]) {0x01}, 1);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, -1);
    expect_bytes(&b, (const u8[]) {0x7f}, 1);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, 63);
    expect_bytes(&b, (const u8[]) {0x3f}, 1);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, 64);
    expect_bytes(&b, (const u8[]) {0xc0, 0x00}, 2);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, -64);
    expect_bytes(&b, (const u8[]) {0x40}, 1);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, -65);
    expect_bytes(&b, (const u8[]) {0xbf, 0x7f}, 2);

    bytebuf_init(&b, a);
    dwarf_sleb128(&b, -123456);
    expect_bytes(&b, (const u8[]) {0xc0, 0xbb, 0x78}, 3);

    arena_free(a);
}

/* The line unit is one version-2 header plus one set_address RELA per function. */
TEST(dwarf_leb, build_line_unit_shape)
{
    Arena *a = arena_new();
    IrModule *m = tc_build_module("int f(void) { return 7; }\n"
                                  "int g(void) { return 9; }\n",
                                  a);
    EXPECT_NOTNULL(m);

    CodegenConfig cfg = {.debug = true};
    CodegenModule *cm = codegen_ir_to_machine(m, &cfg, a);

    DwarfOutput out = {0};
    dwarf_build(cm, "unit.c", "/tmp", &out, a);

    const u8 *line = bytebuf_data(&out.debug_line);
    EXPECT_EQ(line[4], 2); /* version */
    EXPECT_EQ(line[5], 0);
    EXPECT_EQ(vec_size(out.rela_line), 2); /* one address slot per function */

    u32 unit_len =
        (u32) line[0] | ((u32) line[1] << 8) | ((u32) line[2] << 16) | ((u32) line[3] << 24);
    EXPECT_EQ((size_t) unit_len, bytebuf_len(&out.debug_line) - 4);
    EXPECT_TRUE(bytebuf_len(&out.debug_info) > 0);
    EXPECT_TRUE(bytebuf_len(&out.debug_abbrev) > 0);
    /* CU low_pc + one subprogram low_pc per function. */
    EXPECT_EQ(vec_size(out.rela_info), 3);

    arena_free(a);
}
