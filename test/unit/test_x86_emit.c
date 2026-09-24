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

TEST(x86_emit, movq_imm32_sign_extends)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 8, R_EAX, 42, (const u8[]) {0x48, 0xc7, 0xc0, 0x2a, 0, 0, 0}, 7);
    mov_imm(&b, a, 8, R_EAX, -1, (const u8[]) {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff}, 7);
    mov_imm(&b, a, 8, R_EAX, INT32_MIN, (const u8[]) {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x80}, 7);
    mov_imm(&b, a, 8, R_EAX, INT32_MAX, (const u8[]) {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0x7f}, 7);
    mov_imm(&b, a, 8, R_R8, 7, (const u8[]) {0x49, 0xc7, 0xc0, 0x07, 0, 0, 0}, 7);
    arena_free(a);
}

TEST(x86_emit, movq_imm_out_of_range_uses_movabs)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 8, R_EAX, (i64) INT32_MAX + 1,
            (const u8[]) {0x48, 0xb8, 0x00, 0x00, 0x00, 0x80, 0, 0, 0, 0}, 10);
    mov_imm(&b, a, 8, R_EAX, 0x100000000LL,
            (const u8[]) {0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x01, 0, 0, 0}, 10);
    arena_free(a);
}

TEST(x86_emit, mov_zero_stays_a_move_for_flag_safety)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 8, R_EAX, 0, (const u8[]) {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00}, 7);
    arena_free(a);
}

TEST(x86_emit, movl_imml_immw)
{
    Arena *a = arena_new();
    ByteBuf b;
    mov_imm(&b, a, 4, R_ECX, 0x1234, (const u8[]) {0xb9, 0x34, 0x12, 0x00, 0x00}, 5);
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
