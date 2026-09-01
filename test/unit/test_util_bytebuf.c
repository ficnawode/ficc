#include "harness.h"
#include "util/bytebuf.h"

TEST(bytebuf, append_bytes)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    bytebuf_append(&buf, 0xDE);
    bytebuf_append(&buf, 0xAD);
    EXPECT_EQ(bytebuf_len(&buf), 2);
    EXPECT_EQ(bytebuf_data(&buf)[0], 0xDE);
    EXPECT_EQ(bytebuf_data(&buf)[1], 0xAD);
    arena_free(a);
}

TEST(bytebuf, grow_beyond_initial_capacity)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    for (int i = 0; i < 1000; i++)
    {
        bytebuf_append(&buf, (u8) (i & 0xFF));
    }
    EXPECT_EQ(bytebuf_len(&buf), 1000);
    EXPECT_EQ(bytebuf_data(&buf)[999], 0xE7);
    arena_free(a);
}

TEST(bytebuf, append_fixed_width)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    bytebuf_append_u16(&buf, 0x0102);
    bytebuf_append_u32(&buf, 0x01020304);
    bytebuf_append_u64(&buf, 0x0102030405060708ULL);
    EXPECT_EQ(bytebuf_len(&buf), 14);
    EXPECT_EQ(bytebuf_data(&buf)[0], 0x02);
    EXPECT_EQ(bytebuf_data(&buf)[1], 0x01);
    EXPECT_EQ(bytebuf_data(&buf)[2], 0x04);
    EXPECT_EQ(bytebuf_data(&buf)[5], 0x01);
    EXPECT_EQ(bytebuf_data(&buf)[6], 0x08);
    EXPECT_EQ(bytebuf_data(&buf)[13], 0x01);
    arena_free(a);
}

TEST(bytebuf, append_bytes_blob)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    const u8 blob[5] = {1, 2, 3, 4, 5};
    bytebuf_append_bytes(&buf, blob, 5);
    bytebuf_append(&buf, 6);
    EXPECT_EQ(bytebuf_len(&buf), 6);
    EXPECT_EQ(bytebuf_data(&buf)[4], 5);
    EXPECT_EQ(bytebuf_data(&buf)[5], 6);
    arena_free(a);
}

TEST(bytebuf, align_pads_with_zeros)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    bytebuf_append(&buf, 0xAA);
    bytebuf_align(&buf, 8);
    EXPECT_EQ(bytebuf_len(&buf), 8);
    for (size_t i = 1; i < 8; i++)
    {
        EXPECT_EQ(bytebuf_data(&buf)[i], 0);
    }
    bytebuf_align(&buf, 8); /* already aligned: no-op */
    EXPECT_EQ(bytebuf_len(&buf), 8);
    arena_free(a);
}

TEST(bytebuf, poke_u32_overwrites)
{
    Arena *a = arena_new();
    ByteBuf buf;
    bytebuf_init(&buf, a);
    bytebuf_append_i32(&buf, 0);
    bytebuf_append(&buf, 0xAA);
    bytebuf_poke_u32(&buf, 0, 0x01020304);
    EXPECT_EQ(bytebuf_len(&buf), 5);
    EXPECT_EQ(bytebuf_data(&buf)[0], 0x04);
    EXPECT_EQ(bytebuf_data(&buf)[3], 0x01);
    EXPECT_EQ(bytebuf_data(&buf)[4], 0xAA);
    arena_free(a);
}
