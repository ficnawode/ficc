#ifndef FICC_CFI_H
#define FICC_CFI_H

#include "codegen.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct
{
    u64 offset;
    i64 addend;
} CfiReloc;

typedef struct
{
    ByteBuf eh_frame;
    Vec *relocs;
} CfiOutput;

CfiOutput *cfi_build(CodegenModule *cm, Arena *arena);

#endif