#ifndef FICC_LINK_H
#define FICC_LINK_H

#include "cli.h"
#include "elfdefs.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

typedef struct
{
    const char *name;
    u32 type;
    u64 flags;
    u64 align;
    u64 size;
    const u8 *data;
} LinkSection;

typedef struct
{
    const char *name;
    u8 bind;
    u8 type;
    u16 shndx;
    u64 value;
    u64 size;
} LinkSym;

typedef struct
{
    u64 offset;
    u32 sym;
    u32 type;
    i64 addend;
    u32 target_section;
} LinkReloc;

typedef struct LinkObject LinkObject;
struct LinkObject
{
    const char *name;
    Vec *sections;
    Vec *symbols;
    Vec *relocs;
    bool is_dso;
};

LinkObject *link_read_object(const char *path, Arena *arena);
LinkObject *link_read_memory(const u8 *buf, size_t len, const char *name, Arena *arena);

LinkSection *link_find_section(const LinkObject *obj, const char *name);

typedef enum
{
    LINK_INPUT_OBJECT,
    LINK_INPUT_FILE,
} LinkInputKind;

typedef struct
{
    LinkInputKind kind;
    LinkObject *object;
    const char *path;
} LinkInput;

int link_run(const LinkConfig *cfg, Vec *inputs, Arena *arena);

#endif
