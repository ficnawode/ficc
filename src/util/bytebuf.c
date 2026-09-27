#include "bytebuf.h"
#include "assert.h"
#include <string.h>

#define BYTEBUF_INIT_CAP 64

static void bytebuf_grow(ByteBuf *bb, size_t need)
{
    if (bb->len + need > bb->cap)
    {
        while (bb->len + need > bb->cap)
        {
            bb->cap *= 2;
        }
        u8 *old = bb->data;
        bb->data = arena_alloc(bb->arena, bb->cap, 1);
        memcpy(bb->data, old, bb->len);
    }
}

void bytebuf_init(ByteBuf *bb, Arena *arena)
{
    bb->arena = arena;
    bb->len = 0;
    bb->cap = BYTEBUF_INIT_CAP;
    bb->data = arena_alloc(arena, bb->cap, 1);
}

void bytebuf_append(ByteBuf *bb, u8 byte)
{
    bytebuf_grow(bb, 1);
    bb->data[bb->len++] = byte;
}

void bytebuf_append_i8(ByteBuf *bb, i8 val)
{
    bytebuf_append(bb, (u8) val);
}

void bytebuf_append_u16(ByteBuf *bb, u16 val)
{
    bytebuf_append(bb, (u8) (val & 0xFF));
    bytebuf_append(bb, (u8) (val >> 8));
}

void bytebuf_append_u32(ByteBuf *bb, u32 val)
{
    bytebuf_append_u16(bb, (u16) (val & 0xFFFF));
    bytebuf_append_u16(bb, (u16) (val >> 16));
}

void bytebuf_append_i32(ByteBuf *bb, i32 val)
{
    bytebuf_append_u32(bb, (u32) val);
}

void bytebuf_append_u64(ByteBuf *bb, u64 val)
{
    bytebuf_append_u32(bb, (u32) (val & 0xFFFFFFFF));
    bytebuf_append_u32(bb, (u32) (val >> 32));
}

void bytebuf_append_bytes(ByteBuf *bb, const u8 *src, size_t n)
{
    bytebuf_grow(bb, n);
    memcpy(bb->data + bb->len, src, n);
    bb->len += n;
}

void bytebuf_align(ByteBuf *bb, size_t align)
{
    if (align == 0)
    {
        return;
    }
    size_t padded = (bb->len + align - 1) & ~(align - 1);
    while (bb->len < padded)
    {
        bytebuf_append(bb, 0);
    }
}

void bytebuf_poke_u32(ByteBuf *bb, size_t off, u32 val)
{
    ASSERT(off + 4 <= bb->len);
    bb->data[off + 0] = (u8) (val & 0xFF);
    bb->data[off + 1] = (u8) ((val >> 8) & 0xFF);
    bb->data[off + 2] = (u8) ((val >> 16) & 0xFF);
    bb->data[off + 3] = (u8) ((val >> 24) & 0xFF);
}

const u8 *bytebuf_data(const ByteBuf *bb)
{
    return bb->data;
}

size_t bytebuf_len(const ByteBuf *bb)
{
    return bb->len;
}
