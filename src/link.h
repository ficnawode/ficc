#ifndef FICC_LINK_H
#define FICC_LINK_H

#include "cli.h"
#include "elfdefs.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

/* One input section of a relocatable object. `data` is NULL for NOBITS. */
typedef struct
{
    const char *name;
    u32 type;
    u64 flags;
    u64 align;
    u64 size;
    const u8 *data;
} LinkSection;

/* One symbol table entry, decoded from an input `.symtab`. */
typedef struct
{
    const char *name;
    u8 bind;
    u8 type;
    u16 shndx;
    u64 value;
    u64 size;
} LinkSym;

/* One relocation against a section; `target_section` is the input section index. */
typedef struct
{
    u64 offset;
    u32 sym;
    u32 type;
    i64 addend;
    u32 target_section;
} LinkReloc;

/* One input ET_REL object; all storage is arena-backed and borrowed. */
typedef struct LinkObject LinkObject;
struct LinkObject
{
    const char *name; /* path or in-memory label, for diagnostics */
    Vec *sections;    /* Vec<LinkSection*> */
    Vec *symbols;     /* Vec<LinkSym*> */
    Vec *relocs;      /* Vec<Vec<LinkReloc*>*> indexed by target section */
    bool is_dso;      /* ET_DYN input (dynamic linking) */
};

/* Read an ET_REL object from a file or an in-memory image. Returns NULL and
   prints a `[link]` error on a malformed or unsupported object. */
LinkObject *link_read_object(const char *path, Arena *arena);
LinkObject *link_read_memory(const u8 *buf, size_t len, const char *name, Arena *arena);

/* First section with `name`, or NULL. */
LinkSection *link_find_section(const LinkObject *obj, const char *name);

/* One driver-supplied link input: either an already-materialized object
   (compiled in memory) or a path the linker reads itself. */
typedef enum
{
    LINK_INPUT_OBJECT,
    LINK_INPUT_FILE,
} LinkInputKind;

typedef struct
{
    LinkInputKind kind;
    LinkObject *object; /* LINK_INPUT_OBJECT */
    const char *path;   /* LINK_INPUT_FILE */
} LinkInput;

/* Compile-and-link entry: resolve `inputs`, lay out an ET_EXEC, write it to
   `cfg->output_path`. Returns 0 on success, 1 on any `[link]` error. */
int link_run(const LinkConfig *cfg, Vec *inputs, Arena *arena);

#endif
