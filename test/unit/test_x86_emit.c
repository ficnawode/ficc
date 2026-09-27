#include "harness.h"
#include "x86_emit.h"

#include <stdint.h>

static void expect_bytes(ByteBuf *b, const u8 *want, size_t n)
{
    EXPECT_EQ(bytebuf_len(b), n);
    if (bytebuf_len(b) == n)
    {
        EXPECT_EQ(memcmp(bytebuf_data(b), want, n), 0);
    }
}

static void mov_imm(ByteBuf *b, Arena *a, u8 width, u8 reg, i64 imm, const u8 *want, size_t n)
{
    bytebuf_init(b, a);
    emit_mov(b, width, xop_reg(reg), xop_imm(imm));
    expect_bytes(b, want, n);
}

TEST(x86_emit, movq_nonneg_imm_uses_32bit_form)
{
    Arena *a = arena_new();
    ByteBuf b;
    /* B8+rd id writes the 32-bit alias, zero-extending: two bytes shorter than
       the sign-extending C7 form and equivalent for a non-negative value. */
    mov_imm(&b, a, 8, R_EAX, 42, (const u8[]) {0xb8, 0x2a, 0, 0, 0}, 5);
    mov_imm(&b, a, 8, R_EAX, INT32_MAX, (const u8[]) {0xb8, 0xff, 0xff, 0xff, 0x7f}, 5);
    mov_imm(&b, a, 8, R_R8, 7, (const u8[]) {0x41, 0xb8, 0x07, 0, 0, 0}, 6);
    /* A negative value needs the full 64-bit sign extension. */
    mov_imm(&b, a, 8, R_EAX, -1, (const u8[]) {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff}, 7);
    mov_imm(&b, a, 8, R_EAX, INT32_MIN, (const u8[]) {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x80}, 7);
    arena_free(a);
}

TEST(x86_emit, movq_imm_out_of_range_uses_movabs)
{
    Arena *a = arena_new();
    ByteBuf b;
    /* 2^31 still fits the zero-extending 32-bit form. */
    mov_imm(&b, a, 8, R_EAX, (i64) INT32_MAX + 1, (const u8[]) {0xb8, 0x00, 0x00, 0x00, 0x80}, 5);
    mov_imm(&b, a, 8, R_EAX, 0x100000000LL,
            (const u8[]) {0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x01, 0, 0, 0}, 10);
    arena_free(a);
}

TEST(x86_emit, mov_zero_stays_a_move_for_flag_safety)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 8, R_EAX, 0, (const u8[]) {0xb8, 0x00, 0x00, 0x00, 0x00}, 5);
    arena_free(a);
}

TEST(x86_emit, movl_imm_uses_imm32)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 4, R_ECX, 0x1234, (const u8[]) {0xb9, 0x34, 0x12, 0x00, 0x00}, 5);
    arena_free(a);
}

TEST(x86_emit, movw_imm_uses_imm16)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 2, R_EAX, 0x1234, (const u8[]) {0x66, 0xb8, 0x34, 0x12}, 4);
    arena_free(a);
}

TEST(x86_emit, xor_zero_forms)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_xor_zero(&b, 8, R_EAX);
    expect_bytes(&b, (const u8[]) {0x31, 0xc0}, 2);

    bytebuf_init(&b, a);
    emit_xor_zero(&b, 4, R_R8);
    expect_bytes(&b, (const u8[]) {0x45, 0x31, 0xc0}, 3);

    arena_free(a);
}

TEST(x86_emit, inc_dec_forms)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_inc_dec(&b, 8, R_EAX, false);
    expect_bytes(&b, (const u8[]) {0x48, 0xff, 0xc0}, 3);

    bytebuf_init(&b, a);
    emit_inc_dec(&b, 4, R_R14, false);
    expect_bytes(&b, (const u8[]) {0x41, 0xff, 0xc6}, 3);

    bytebuf_init(&b, a);
    emit_inc_dec(&b, 8, R_EAX, true);
    expect_bytes(&b, (const u8[]) {0x48, 0xff, 0xc8}, 3);

    bytebuf_init(&b, a);
    emit_inc_dec(&b, 1, 4, false);
    expect_bytes(&b, (const u8[]) {0x40, 0xfe, 0xc4}, 3);

    arena_free(a);
}

TEST(x86_emit, test_zero_uses_test)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_test_reg(&b, 8, R_EAX);
    expect_bytes(&b, (const u8[]) {0x48, 0x85, 0xc0}, 3);

    bytebuf_init(&b, a);
    emit_test_reg(&b, 4, R_EAX);
    expect_bytes(&b, (const u8[]) {0x85, 0xc0}, 2);

    bytebuf_init(&b, a);
    emit_test_reg(&b, 8, R_R12);
    expect_bytes(&b, (const u8[]) {0x4d, 0x85, 0xe4}, 3);

    bytebuf_init(&b, a);
    emit_test_reg(&b, 1, R_EDI);
    expect_bytes(&b, (const u8[]) {0x40, 0x84, 0xff}, 3);

    arena_free(a);
}

TEST(x86_emit, mem_operand_extended_bases)
{
    Arena *a = arena_new();
    ByteBuf b;

    /* %r12 has rm field 4, so it always needs a SIB byte. */
    bytebuf_init(&b, a);
    emit_mov(&b, 4, xop_reg(R_EAX),
             xop_mem((X86Mem) {.base = R_R12, .index = NO_REG, .scale = 1, .disp = 0}));
    expect_bytes(&b, (const u8[]) {0x41, 0x8b, 0x04, 0x24}, 4);

    /* %r13 has base field 5, so disp=0 must use mod=1 with a zero disp8. */
    bytebuf_init(&b, a);
    emit_mov(&b, 4, xop_reg(R_EAX),
             xop_mem((X86Mem) {.base = R_R13, .index = NO_REG, .scale = 1, .disp = 0}));
    expect_bytes(&b, (const u8[]) {0x41, 0x8b, 0x45, 0x00}, 4);

    bytebuf_init(&b, a);
    emit_mov(&b, 4, xop_reg(R_EAX),
             xop_mem((X86Mem) {.base = R_R12, .index = R_ECX, .scale = 4, .disp = 0}));
    expect_bytes(&b, (const u8[]) {0x41, 0x8b, 0x04, 0x8c}, 4);

    arena_free(a);
}

TEST(x86_emit, shift_immediate_forms)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 8, R_EAX, 4, 3);
    expect_bytes(&b, (const u8[]) {0x48, 0xc1, 0xe0, 0x03}, 4);

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 8, R_R11, 4, 1);
    expect_bytes(&b, (const u8[]) {0x49, 0xc1, 0xe3, 0x01}, 4);

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 4, R_EAX, 5, 2);
    expect_bytes(&b, (const u8[]) {0xc1, 0xe8, 0x02}, 3);

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 1, R_EAX, 4, 1);
    expect_bytes(&b, (const u8[]) {0xc0, 0xe0, 0x01}, 3);

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 1, 4, 4, 1); /* spl needs a REX prefix */
    expect_bytes(&b, (const u8[]) {0x40, 0xc0, 0xe4, 0x01}, 4);

    bytebuf_init(&b, a);
    emit_shift_imm(&b, 8, R_EAX, 7, 3); /* C1 /7 is SAR */
    expect_bytes(&b, (const u8[]) {0x48, 0xc1, 0xf8, 0x03}, 4);

    arena_free(a);
}

TEST(x86_emit, shift_by_cl_uses_the_count_register)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_shift_cl(&b, 8, R_EAX, 4);
    expect_bytes(&b, (const u8[]) {0x48, 0xd3, 0xe0}, 3);

    arena_free(a);
}

TEST(x86_emit, lea_of_the_same_register_is_elided)
{
    Arena *a = arena_new();
    ByteBuf b;
    bytebuf_init(&b, a);
    emit_lea(&b, R_ECX, (X86Mem) {.base = R_ECX, .index = NO_REG, .scale = 1, .disp = 0});
    expect_bytes(&b, (const u8[]) {0}, 0);
    bytebuf_init(&b, a);
    emit_lea(&b, R_ECX, (X86Mem) {.base = R_ECX, .index = NO_REG, .scale = 1, .disp = 8});
    expect_bytes(&b, (const u8[]) {0x48, 0x8d, 0x49, 0x08}, 4);
    arena_free(a);
}

TEST(x86_emit, reg_reg_extends_both_operands)
{
    Arena *a = arena_new();
    ByteBuf b;

    bytebuf_init(&b, a);
    emit_reg_reg(&b, 0x03, R_EAX, R_EAX);
    expect_bytes(&b, (const u8[]) {0x48, 0x03, 0xc0}, 3);

    bytebuf_init(&b, a);
    emit_reg_reg(&b, 0x03, R_EAX, R_R11);
    expect_bytes(&b, (const u8[]) {0x49, 0x03, 0xc3}, 3);

    bytebuf_init(&b, a);
    emit_reg_reg(&b, 0x03, R_R12, R_EAX);
    expect_bytes(&b, (const u8[]) {0x4c, 0x03, 0xe0}, 3);

    arena_free(a);
}

TEST(x86_emit, byte_mov_omits_a_noop_rex)
{
    Arena *a = arena_new();
    ByteBuf b;

    /* al/cl/dl/bl encode without a prefix; a bare 0x40 would be a wasted byte. */
    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_EAX), xop_imm(42));
    expect_bytes(&b, (const u8[]) {0xb0, 0x2a}, 2);

    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_EAX), xop_reg(R_ECX));
    expect_bytes(&b, (const u8[]) {0x88, 0xc8}, 2);

    X86Mem rbx0 = {.base = R_EBX, .index = NO_REG, .scale = 1, .disp = 0};
    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_mem(rbx0), xop_reg(R_EAX));
    expect_bytes(&b, (const u8[]) {0x88, 0x03}, 2);

    /* spl/bpl/sil/dil still need REX (4-7 map to ah/ch/dh/bh otherwise). */
    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_ESP), xop_imm(42));
    expect_bytes(&b, (const u8[]) {0x40, 0xb4, 0x2a}, 3);

    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_ESP), xop_reg(R_EAX));
    expect_bytes(&b, (const u8[]) {0x40, 0x88, 0xc4}, 3);

    /* r8b-r15b need the REX bits. */
    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_R8), xop_imm(42));
    expect_bytes(&b, (const u8[]) {0x41, 0xb0, 0x2a}, 3);

    bytebuf_init(&b, a);
    emit_mov(&b, 1, xop_reg(R_EAX), xop_reg(R_R8));
    expect_bytes(&b, (const u8[]) {0x44, 0x88, 0xc0}, 3);

    arena_free(a);
}
