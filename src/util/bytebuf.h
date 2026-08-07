#ifndef FICC_BYTEBUF_H
#define FICC_BYTEBUF_H

#include "arena.h"
#include "types.h"

typedef struct ByteBuf ByteBuf;
struct ByteBuf
{
    u8 *data;
    size_t len;
    size_t cap;
    Arena *arena;
};

void bytebuf_init(ByteBuf *bb, Arena *arena);
void bytebuf_append(ByteBuf *bb, u8 byte);
void bytebuf_append_i8(ByteBuf *bb, i8 val);
void bytebuf_append_u16(ByteBuf *bb, u16 val);
void bytebuf_append_i32(ByteBuf *bb, i32 val);
void bytebuf_append_u32(ByteBuf *bb, u32 val);
void bytebuf_append_u64(ByteBuf *bb, u64 val);
void bytebuf_append_bytes(ByteBuf *bb, const u8 *src, size_t n);
void bytebuf_align(ByteBuf *bb, size_t align);
void bytebuf_poke_u32(ByteBuf *bb, size_t off, u32 val);
u8 *bytebuf_data(ByteBuf *bb);
size_t bytebuf_len(const ByteBuf *bb);

#endif
