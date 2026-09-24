#include "link.h"
#include "util/hashmap.h"
#include "x86_link.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void link_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[link] error: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void link_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[link] warning: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static bool in_range(u64 len, u64 off, u64 size)
{
    return off <= len && size <= len - off;
}

static const char *cstr_at(const u8 *buf, size_t len, u64 off)
{
    if (off >= len)
    {
        return NULL;
    }
    size_t i = (size_t) off;
    while (i < len && buf[i] != 0)
    {
        i++;
    }
    if (i >= len)
    {
        return NULL;
    }
    return (const char *) (buf + off);
}

static LinkObject *object_new(const char *name, Arena *arena)
{
    LinkObject *obj = arena_alloc(arena, sizeof(*obj), sizeof(void *));
    obj->name = name;
    obj->sections = vec_new(arena);
    obj->symbols = vec_new(arena);
    obj->relocs = vec_new(arena);
    obj->is_dso = false;
    return obj;
}

static const Elf64_Shdr *shdr_at(const u8 *buf, size_t len, const Elf64_Ehdr *eh, u64 i)
{
    if (i >= eh->e_shnum)
    {
        return NULL;
    }
    if (!in_range(len, eh->e_shoff + i * eh->e_shentsize, sizeof(Elf64_Shdr)))
    {
        return NULL;
    }
    return (const Elf64_Shdr *) (buf + eh->e_shoff + i * eh->e_shentsize);
}

static bool read_sections(const u8 *buf, size_t len, const Elf64_Ehdr *eh, LinkObject *obj,
                          Arena *arena)
{
    if (eh->e_shstrndx >= eh->e_shnum)
    {
        link_error("%s: bad section-name string table index", obj->name);
        return false;
    }
    const Elf64_Shdr *shstr = shdr_at(buf, len, eh, eh->e_shstrndx);
    if (!shstr || !in_range(len, shstr->sh_offset, shstr->sh_size))
    {
        link_error("%s: bad section-name string table", obj->name);
        return false;
    }
    const u8 *strtab = buf + shstr->sh_offset;
    size_t strtab_len = (size_t) shstr->sh_size;

    for (u64 i = 0; i < eh->e_shnum; i++)
    {
        const Elf64_Shdr *sh = shdr_at(buf, len, eh, i);
        if (!sh)
        {
            link_error("%s: section header %llu out of range", obj->name, (unsigned long long) i);
            return false;
        }
        const char *name = cstr_at(strtab, strtab_len, sh->sh_name);
        if (!name)
        {
            link_error("%s: section %llu has an invalid name", obj->name, (unsigned long long) i);
            return false;
        }
        LinkSection *sec = arena_alloc(arena, sizeof(*sec), sizeof(void *));
        sec->name = name;
        sec->type = sh->sh_type;
        sec->flags = sh->sh_flags;
        sec->align = sh->sh_addralign ? sh->sh_addralign : 1;
        sec->size = sh->sh_size;
        sec->data = NULL;
        if (sh->sh_type != SHT_NOBITS && sh->sh_size > 0)
        {
            if (!in_range(len, sh->sh_offset, sh->sh_size))
            {
                link_error("%s: section '%s' data out of range", obj->name, name);
                return false;
            }
            sec->data = buf + sh->sh_offset;
        }
        vec_push(obj->sections, sec);
    }
    return true;
}

static bool read_symbols(const u8 *buf, size_t len, const Elf64_Ehdr *eh, LinkObject *obj,
                         Arena *arena)
{
    for (u64 i = 0; i < eh->e_shnum; i++)
    {
        const Elf64_Shdr *sh = shdr_at(buf, len, eh, i);
        if (!sh || sh->sh_type != SHT_SYMTAB)
        {
            continue;
        }
        if (sh->sh_entsize < sizeof(Elf64_Sym) || sh->sh_link >= eh->e_shnum)
        {
            link_error("%s: malformed .symtab", obj->name);
            return false;
        }
        const Elf64_Shdr *str = shdr_at(buf, len, eh, sh->sh_link);
        if (!str || !in_range(len, str->sh_offset, str->sh_size))
        {
            link_error("%s: malformed .strtab", obj->name);
            return false;
        }
        const u8 *strtab = buf + str->sh_offset;
        size_t strtab_len = (size_t) str->sh_size;
        u64 nsyms = sh->sh_size / sh->sh_entsize;
        for (u64 s = 0; s < nsyms; s++)
        {
            const u8 *p = buf + sh->sh_offset + s * sh->sh_entsize;
            Elf64_Sym es;
            memcpy(&es, p, sizeof(es));
            LinkSym *sym = arena_alloc(arena, sizeof(*sym), sizeof(void *));
            sym->name = cstr_at(strtab, strtab_len, es.st_name);
            if (!sym->name)
            {
                sym->name = "";
            }
            sym->bind = ELF64_ST_BIND(es.st_info);
            sym->type = ELF64_ST_TYPE(es.st_info);
            sym->shndx = es.st_shndx;
            sym->value = es.st_value;
            sym->size = es.st_size;
            vec_push(obj->symbols, sym);
        }
        return true;
    }
    return true;
}

static bool read_relocations(const u8 *buf, size_t len, const Elf64_Ehdr *eh, LinkObject *obj,
                             Arena *arena)
{
    for (u64 i = 0; i < eh->e_shnum; i++)
    {
        vec_push(obj->relocs, vec_new(arena));
    }
    for (u64 i = 0; i < eh->e_shnum; i++)
    {
        const Elf64_Shdr *sh = shdr_at(buf, len, eh, i);
        if (!sh || sh->sh_type != SHT_RELA)
        {
            continue;
        }
        if (sh->sh_info >= eh->e_shnum || sh->sh_entsize < sizeof(Elf64_Rela))
        {
            link_error("%s: malformed relocation section", obj->name);
            return false;
        }
        Vec *list = (Vec *) vec_get(obj->relocs, sh->sh_info);
        u64 nrel = sh->sh_size / sh->sh_entsize;
        for (u64 r = 0; r < nrel; r++)
        {
            const u8 *p = buf + sh->sh_offset + r * sh->sh_entsize;
            Elf64_Rela er;
            memcpy(&er, p, sizeof(er));
            u32 sym = (u32) ELF64_R_SYM(er.r_info);
            if (sym >= vec_size(obj->symbols))
            {
                link_error("%s: relocation symbol index out of range", obj->name);
                return false;
            }
            LinkReloc *rel = arena_alloc(arena, sizeof(*rel), sizeof(void *));
            rel->offset = er.r_offset;
            rel->sym = sym;
            rel->type = (u32) ELF64_R_TYPE(er.r_info);
            rel->addend = er.r_addend;
            rel->target_section = sh->sh_info;
            vec_push(list, rel);
        }
    }
    return true;
}

LinkObject *link_read_memory(const u8 *buf, size_t len, const char *name, Arena *arena)
{
    const LinkArch *arch = x86_link_arch();
    if (len < sizeof(Elf64_Ehdr))
    {
        link_error("%s: truncated ELF header", name);
        return NULL;
    }
    Elf64_Ehdr eh;
    memcpy(&eh, buf, sizeof(eh));
    if (eh.e_ident[0] != ELFMAG0 || eh.e_ident[1] != ELFMAG1 || eh.e_ident[2] != ELFMAG2 ||
        eh.e_ident[3] != ELFMAG3)
    {
        link_error("%s: not an ELF file", name);
        return NULL;
    }
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB)
    {
        link_error("%s: not a 64-bit little-endian ELF file", name);
        return NULL;
    }
    if (eh.e_machine != arch->machine)
    {
        link_error("%s: wrong machine (want EM_X86_64)", name);
        return NULL;
    }
    if (eh.e_type != ET_REL)
    {
        link_error("%s: not a relocatable object", name);
        return NULL;
    }
    if (eh.e_shentsize < sizeof(Elf64_Shdr) || eh.e_shnum == 0)
    {
        link_error("%s: missing section headers", name);
        return NULL;
    }

    LinkObject *obj = object_new(name, arena);
    if (!read_sections(buf, len, &eh, obj, arena) || !read_symbols(buf, len, &eh, obj, arena) ||
        !read_relocations(buf, len, &eh, obj, arena))
    {
        return NULL;
    }
    return obj;
}

static u8 *read_whole_file(const char *path, size_t *out_len, Arena *arena)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        link_error("%s: cannot open input", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        link_error("%s: cannot seek input", path);
        return NULL;
    }
    long end = ftell(f);
    if (end < 0)
    {
        fclose(f);
        link_error("%s: cannot size input", path);
        return NULL;
    }
    rewind(f);
    u8 *buf = arena_alloc(arena, (size_t) end + 1, 16);
    size_t n = fread(buf, 1, (size_t) end, f);
    fclose(f);
    if (n != (size_t) end)
    {
        link_error("%s: short read", path);
        return NULL;
    }
    *out_len = n;
    return buf;
}

LinkObject *link_read_object(const char *path, Arena *arena)
{
    size_t len = 0;
    u8 *buf = read_whole_file(path, &len, arena);
    if (!buf)
    {
        return NULL;
    }
    return link_read_memory(buf, len, path, arena);
}

LinkSection *link_find_section(const LinkObject *obj, const char *name)
{
    size_t n = vec_size(obj->sections);
    for (size_t i = 0; i < n; i++)
    {
        LinkSection *sec = (LinkSection *) vec_get(obj->sections, i);
        if (strcmp(sec->name, name) == 0)
        {
            return sec;
        }
    }
    return NULL;
}

#define LINK_PAGE 0x1000
#define LINK_BASE 0x400000

static u64 align_up(u64 n, u64 a)
{
    return (n + a - 1) / a * a;
}

typedef enum
{
    OUT_NULL = 0,
    OUT_TEXT,
    OUT_RODATA,
    OUT_DATA,
    OUT_BSS,
    OUT_INIT_ARRAY,
    OUT_FINI_ARRAY,
    OUT_EH_FRAME,
    OUT_GCC_EXCEPT,
    OUT_COUNT
} OutSecId;

#define CLASS_IGNORE (-1)
#define CLASS_UNSUPPORTED (-2)

typedef struct
{
    const char *name;
    u32 type;
    u64 flags;
    u64 align;
    u64 entsize;
} OutSecDesc;

static const OutSecDesc OUT_SECS[OUT_COUNT] = {
    {".", SHT_NULL, 0, 1, 0},
    {".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16, 0},
    {".rodata", SHT_PROGBITS, SHF_ALLOC, 8, 0},
    {".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 8, 0},
    {".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 8, 0},
    {".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE, 8, 8},
    {".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE, 8, 8},
    {".eh_frame", SHT_PROGBITS, SHF_ALLOC, 8, 0},
    {".gcc_except_table", SHT_PROGBITS, SHF_ALLOC, 4, 0},
};

typedef struct
{
    ByteBuf bytes;
    u64 size; /* logical size; == bytes.len for non-NOBITS */
    u64 align;
    u64 addr;
    u64 offset;
} OutSec;

typedef struct
{
    LinkObject *obj;
    u64 *sec_out; /* OutSecId per input section, OUT_NULL when ignored */
    u64 *sec_off; /* offset of each input section within its output section */
} InputObject;

typedef struct GlobalSym GlobalSym;
struct GlobalSym
{
    const char *name;
    u8 type;
    bool is_defined;
    bool is_weak;
    bool is_common;
    bool is_abs;
    u64 out_sec;
    u64 value;
    u64 common_size;
    u64 common_align;
    u64 addr;
};

typedef struct
{
    const char *name;
    u8 type, bind;
    u64 value, size;
    u16 shndx;
} DsoSym;

typedef struct
{
    const char *path;
    const char *soname;
    const u8 *data;
    size_t len;
    StrMap *exports; /* name -> DsoSym* */
} Dso;

typedef struct
{
    const char *name;
    u8 info;
    u16 shndx;
    u64 value;
    u64 size;
    u32 name_off;
} DynSym;

typedef struct
{
    u64 offset;
    u32 sym;
    u32 type;
    i64 addend;
    void *owner;
} DynReloc;

typedef struct
{
    const char *name;
    u64 off; /* offset within .got or .got.plt */
    u64 addr;
    bool is_dynamic; /* slot filled by ld.so via GLOB_DAT */
} GotSlot;

typedef struct
{
    const char *name;
    u64 plt_off;
    u64 got_off;
    u64 plt_addr;
    u64 got_addr;
    u32 dynidx;
} PltSlot;

typedef struct
{
    const char *name;
    u64 size;
    u64 align;
    u64 off; /* offset within .bss */
    u64 addr;
    u32 dynidx;
    bool has_reloc;
} CopySlot;

#define DSEC_COUNT 12
typedef enum
{
    DSEC_INTERP = 0,
    DSEC_HASH,
    DSEC_DYNSYM,
    DSEC_DYNSTR,
    DSEC_VER,
    DSEC_VERNEED,
    DSEC_RELA_DYN,
    DSEC_RELA_PLT,
    DSEC_PLT,
    DSEC_DYNAMIC,
    DSEC_GOT,
    DSEC_GOT_PLT,
} DynSecId;

typedef struct
{
    const char *name;
    u32 type;
    u64 flags;
    u64 align;
    u64 entsize;
    int seg;
} DynSecDesc;

static const DynSecDesc DYN_SECS[DSEC_COUNT] = {
    {".interp", SHT_PROGBITS, SHF_ALLOC, 1, 0, 0},
    {".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, 8, 0, 0},
    {".dynsym", SHT_DYNSYM, SHF_ALLOC, 8, sizeof(Elf64_Sym), 0},
    {".dynstr", SHT_STRTAB, SHF_ALLOC, 1, 0, 0},
    {".gnu.version", SHT_GNU_VERSYM, SHF_ALLOC, 2, 2, 0},
    {".gnu.version_r", SHT_GNU_VERNEED, SHF_ALLOC, 8, 0, 0},
    {".rela.dyn", SHT_RELA, SHF_ALLOC, 8, sizeof(Elf64_Rela), 0},
    {".rela.plt", SHT_RELA, SHF_ALLOC, 8, sizeof(Elf64_Rela), 0},
    {".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 16, 16, 1},
    {".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 8, sizeof(Elf64_Dyn), 3},
    {".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 8, 8, 3},
    {".got.plt", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 8, 8, 3},
};

typedef struct
{
    const LinkConfig *cfg;
    Arena *arena;
    Vec *objects; /* Vec<InputObject*> */
    OutSec out[OUT_COUNT];
    StrMap *globals;
    Vec *global_order;    /* Vec<GlobalSym*>, creation order (deterministic) */
    Vec *archives;        /* Vec<Archive*> */
    Vec *dsos;            /* Vec<Dso*> */
    Vec *search_dirs;     /* Vec<const char*> — -L dirs then the built-in list */
    StrMap *loaded_paths; /* file inputs already added, by path */
    size_t nresolved;     /* objects below this index are already in the global table */
    u64 seg_off[4], seg_addr[4], seg_filesz[4], seg_memsz[4];
    u64 entry;
    bool has_start_stub;
    u64 start_off;
    u64 start_rel_off;
    u32 nerrors;

    /* Dynamic output state (used only when `dynamic`). */
    bool dynamic;
    Vec *dynsyms;      /* Vec<DynSym*> — index 0 is null */
    StrMap *dyn_index; /* name -> DynSym* */
    Vec *rela_dyn;     /* Vec<DynReloc*> */
    Vec *rela_plt;     /* Vec<DynReloc*> */
    Vec *needed;       /* Vec<const char*> sonames */
    Vec *needed_off;   /* Vec<u64> offsets of sonames in .dynstr */
    StrMap *got_map;   /* name -> GotSlot* */
    Vec *got_slots;    /* Vec<GotSlot*> */
    StrMap *plt_map;   /* name -> PltSlot* */
    Vec *plt_slots;    /* Vec<PltSlot*> */
    Vec *copies;       /* Vec<CopySlot*> */
    ByteBuf dynbuf[DSEC_COUNT];
    u64 dyn_addr[DSEC_COUNT];
    u64 dyn_off[DSEC_COUNT];
    u64 got_plt_size;
} Linker;

static void linker_init(Linker *lk, const LinkConfig *cfg, Arena *arena)
{
    lk->cfg = cfg;
    lk->arena = arena;
    lk->objects = vec_new(arena);
    lk->globals = strmap_new(arena);
    lk->global_order = vec_new(arena);
    lk->archives = vec_new(arena);
    lk->dsos = vec_new(arena);
    lk->search_dirs = vec_new(arena);
    lk->loaded_paths = strmap_new(arena);
    lk->nresolved = 0;
    lk->nerrors = 0;
    lk->dynamic = false;
    lk->dynsyms = vec_new(arena);
    lk->dyn_index = strmap_new(arena);
    lk->rela_dyn = vec_new(arena);
    lk->rela_plt = vec_new(arena);
    lk->needed = vec_new(arena);
    lk->needed_off = vec_new(arena);
    lk->got_map = strmap_new(arena);
    lk->got_slots = vec_new(arena);
    lk->plt_map = strmap_new(arena);
    lk->plt_slots = vec_new(arena);
    lk->copies = vec_new(arena);
    lk->got_plt_size = 0;
    for (int i = 0; i < DSEC_COUNT; i++)
    {
        bytebuf_init(&lk->dynbuf[i], arena);
        lk->dyn_addr[i] = 0;
        lk->dyn_off[i] = 0;
    }
    for (int i = 0; i < OUT_COUNT; i++)
    {
        bytebuf_init(&lk->out[i].bytes, arena);
        lk->out[i].size = 0;
        lk->out[i].align = OUT_SECS[i].align;
        lk->out[i].addr = 0;
        lk->out[i].offset = 0;
    }
}

static bool name_is(const char *s, const char *lit)
{
    return strcmp(s, lit) == 0;
}

static bool name_has_prefix(const char *s, const char *base)
{
    size_t n = strlen(base);
    if (strncmp(s, base, n) != 0)
    {
        return false;
    }
    return s[n] == '\0' || s[n] == '.';
}

static int classify_section(const LinkSection *sec)
{
    const char *n = sec->name;
    if (name_has_prefix(n, ".sframe"))
    {
        return CLASS_IGNORE;
    }
    switch (sec->type)
    {
        case SHT_PROGBITS:
        case SHT_NOBITS:
        case SHT_INIT_ARRAY:
        case SHT_FINI_ARRAY:
        case SHT_PREINIT_ARRAY:
            break;
        case SHT_NOTE:
            return CLASS_IGNORE;
        default:
            if (sec->flags & SHF_ALLOC)
            {
                return CLASS_UNSUPPORTED;
            }
            return CLASS_IGNORE;
    }
    if (name_has_prefix(n, ".text"))
    {
        return OUT_TEXT;
    }
    if (name_has_prefix(n, ".rodata"))
    {
        return OUT_RODATA;
    }
    if (name_has_prefix(n, ".data"))
    {
        return OUT_DATA;
    }
    if (name_has_prefix(n, ".bss") || name_has_prefix(n, ".sbss"))
    {
        return OUT_BSS;
    }
    if (name_has_prefix(n, ".init_array"))
    {
        return OUT_INIT_ARRAY;
    }
    if (name_has_prefix(n, ".fini_array"))
    {
        return OUT_FINI_ARRAY;
    }
    if (name_has_prefix(n, ".eh_frame"))
    {
        return OUT_EH_FRAME;
    }
    if (name_has_prefix(n, ".gcc_except_table"))
    {
        return OUT_GCC_EXCEPT;
    }
    if (name_has_prefix(n, ".tdata") || name_has_prefix(n, ".tbss"))
    {
        return CLASS_UNSUPPORTED;
    }
    if (name_has_prefix(n, ".tm_clone_table") || name_has_prefix(n, ".note"))
    {
        return CLASS_IGNORE;
    }
    if (sec->flags & SHF_ALLOC)
    {
        return CLASS_UNSUPPORTED;
    }
    return CLASS_IGNORE;
}

static u64 out_append(Linker *lk, OutSecId id, const LinkSection *sec)
{
    OutSec *o = &lk->out[id];
    u64 off = align_up(o->size, sec->align);
    if (sec->type != SHT_NOBITS && sec->size > 0)
    {
        while (o->bytes.len < off)
        {
            bytebuf_append(&o->bytes, 0);
        }
        bytebuf_append_bytes(&o->bytes, sec->data, sec->size);
    }
    o->size = off + sec->size;
    if (sec->align > o->align)
    {
        o->align = sec->align;
    }
    return off;
}

static bool merge_object(Linker *lk, LinkObject *obj)
{
    u64 nsec = vec_size(obj->sections);
    InputObject *io = arena_alloc(lk->arena, sizeof(*io), sizeof(void *));
    io->obj = obj;
    io->sec_out = arena_alloc(lk->arena, nsec * sizeof(u64), sizeof(void *));
    io->sec_off = arena_alloc(lk->arena, nsec * sizeof(u64), sizeof(void *));
    for (u64 i = 0; i < nsec; i++)
    {
        io->sec_out[i] = OUT_NULL;
        io->sec_off[i] = 0;
    }
    for (u64 i = 0; i < nsec; i++)
    {
        LinkSection *sec = (LinkSection *) vec_get(obj->sections, i);
        int cls = classify_section(sec);
        if (cls == CLASS_IGNORE)
        {
            continue;
        }
        if (cls == CLASS_UNSUPPORTED)
        {
            link_error("%s: unsupported section '%s'", obj->name, sec->name);
            return false;
        }
        io->sec_out[i] = (u64) cls;
        io->sec_off[i] = out_append(lk, (OutSecId) cls, sec);
    }
    vec_push(lk->objects, io);
    return true;
}

static GlobalSym *global_create(Linker *lk, const char *name)
{
    GlobalSym *g = arena_alloc(lk->arena, sizeof(*g), sizeof(void *));
    g->name = name;
    g->type = STT_NOTYPE;
    g->is_defined = false;
    g->is_weak = false;
    g->is_common = false;
    g->is_abs = false;
    g->out_sec = OUT_NULL;
    g->value = 0;
    g->common_size = 0;
    g->common_align = 0;
    g->addr = 0;
    strmap_set(lk->globals, name, g);
    vec_push(lk->global_order, g);
    return g;
}

static void define_global(Linker *lk, const char *name, LinkSym *def, u64 out_sec, u64 value,
                          bool is_abs)
{
    bool weak = def->bind == STB_WEAK;
    GlobalSym *g = strmap_get(lk->globals, name);
    if (!g)
    {
        g = global_create(lk, name);
    }
    else if (g->is_defined)
    {
        if (!g->is_weak && !weak)
        {
            link_error("duplicate definition of '%s'", name);
            lk->nerrors++;
            return;
        }
        if (weak)
        {
            return;
        }
    }
    g->is_defined = true;
    g->is_weak = weak;
    g->is_common = false;
    g->is_abs = is_abs;
    g->out_sec = out_sec;
    g->value = value;
    g->type = def->type;
}

static void define_common(Linker *lk, const char *name, LinkSym *def)
{
    GlobalSym *g = strmap_get(lk->globals, name);
    if (!g)
    {
        g = global_create(lk, name);
    }
    if (g->is_defined)
    {
        return;
    }
    g->is_common = true;
    if (def->size > g->common_size)
    {
        g->common_size = def->size;
    }
    if (def->value > g->common_align)
    {
        g->common_align = def->value;
    }
}

static void allocate_commons(Linker *lk)
{
    OutSec *bss = &lk->out[OUT_BSS];
    for (size_t i = 0; i < vec_size(lk->global_order); i++)
    {
        GlobalSym *g = (GlobalSym *) vec_get(lk->global_order, i);
        if (!g->is_common || g->is_defined)
        {
            continue;
        }
        u64 a = g->common_align ? g->common_align : 1;
        u64 off = align_up(bss->size, a);
        g->out_sec = OUT_BSS;
        g->value = off;
        g->is_defined = true;
        bss->size = off + g->common_size;
        if (a > bss->align)
        {
            bss->align = a;
        }
    }
}

static void resolve_globals(Linker *lk)
{
    for (size_t oi = lk->nresolved; oi < vec_size(lk->objects); oi++)
    {
        InputObject *io = (InputObject *) vec_get(lk->objects, oi);
        u64 nsyms = vec_size(io->obj->symbols);
        for (u64 si = 0; si < nsyms; si++)
        {
            LinkSym *s = (LinkSym *) vec_get(io->obj->symbols, si);
            if (s->bind == STB_LOCAL || s->shndx == SHN_UNDEF)
            {
                continue;
            }
            if (s->shndx == SHN_COMMON)
            {
                define_common(lk, s->name, s);
                continue;
            }
            if (s->shndx == SHN_ABS)
            {
                define_global(lk, s->name, s, OUT_NULL, s->value, true);
                continue;
            }
            if (s->shndx >= vec_size(io->obj->sections))
            {
                link_error("%s: symbol '%s' has a bad section index", io->obj->name, s->name);
                lk->nerrors++;
                continue;
            }
            u64 out_id = io->sec_out[s->shndx];
            if (out_id == OUT_NULL)
            {
                continue;
            }
            define_global(lk, s->name, s, out_id, io->sec_off[s->shndx] + s->value, false);
        }
    }
    lk->nresolved = vec_size(lk->objects);
    allocate_commons(lk);
}

static DsoSym *dso_lookup(Linker *lk, const char *name)
{
    for (size_t i = 0; i < vec_size(lk->dsos); i++)
    {
        Dso *d = (Dso *) vec_get(lk->dsos, i);
        DsoSym *s = (DsoSym *) strmap_get(d->exports, name);
        if (s)
        {
            return s;
        }
    }
    return NULL;
}

static u32 dynsym_add(Linker *lk, const char *name, u8 info, u16 shndx, u64 value, u64 size)
{
    void *v = strmap_get(lk->dyn_index, name);
    if (v)
    {
        return (u32) (uintptr_t) v - 1;
    }
    DynSym *d = arena_alloc(lk->arena, sizeof(*d), sizeof(void *));
    d->name = name;
    d->info = info;
    d->shndx = shndx;
    d->value = value;
    d->size = size;
    vec_push(lk->dynsyms, d);
    u32 idx = (u32) (vec_size(lk->dynsyms) - 1);
    strmap_set(lk->dyn_index, name, (void *) (uintptr_t) (idx + 1));
    return idx;
}

static GotSlot *got_slot(Linker *lk, const char *name)
{
    GotSlot *g = (GotSlot *) strmap_get(lk->got_map, name);
    if (g)
    {
        return g;
    }
    g = arena_alloc(lk->arena, sizeof(*g), sizeof(void *));
    g->name = name;
    g->off = vec_size(lk->got_slots) * 8;
    g->addr = 0;
    g->is_dynamic = false;
    vec_push(lk->got_slots, g);
    strmap_set(lk->got_map, name, g);
    return g;
}

static PltSlot *plt_slot(Linker *lk, const char *name)
{
    PltSlot *p = (PltSlot *) strmap_get(lk->plt_map, name);
    if (p)
    {
        return p;
    }
    p = arena_alloc(lk->arena, sizeof(*p), sizeof(void *));
    p->name = name;
    p->plt_off = vec_size(lk->plt_slots) * 16;
    p->got_off = (3 + vec_size(lk->plt_slots)) * 8;
    p->plt_addr = 0;
    p->got_addr = 0;
    p->dynidx = dynsym_add(lk, name, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), SHN_UNDEF, 0, 0);
    vec_push(lk->plt_slots, p);
    strmap_set(lk->plt_map, name, p);
    DynReloc *r = arena_alloc(lk->arena, sizeof(*r), sizeof(void *));
    r->offset = 0;
    r->sym = p->dynidx;
    r->type = R_X86_64_JUMP_SLOT;
    r->addend = 0;
    vec_push(lk->rela_plt, r);
    return p;
}

static CopySlot *copy_slot(Linker *lk, const char *name, u64 size, u64 align)
{
    for (size_t i = 0; i < vec_size(lk->copies); i++)
    {
        CopySlot *c = (CopySlot *) vec_get(lk->copies, i);
        if (strcmp(c->name, name) == 0)
        {
            return c;
        }
    }
    CopySlot *c = arena_alloc(lk->arena, sizeof(*c), sizeof(void *));
    c->name = name;
    c->size = size;
    c->align = align ? align : 8;
    OutSec *bss = &lk->out[OUT_BSS];
    c->off = align_up(bss->size, c->align);
    bss->size = c->off + size;
    if (c->align > bss->align)
    {
        bss->align = c->align;
    }
    c->addr = 0;
    c->dynidx = dynsym_add(lk, name, ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT), SHN_UNDEF, 0, size);
    c->has_reloc = false;
    vec_push(lk->copies, c);
    return c;
}

static bool is_gotpcrel(u32 type)
{
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX ||
           type == R_X86_64_REX_GOTPCRELX;
}

static bool synth_value(Linker *lk, const char *name, u64 *out)
{
    OutSec *bss = &lk->out[OUT_BSS];
    if (lk->dynamic && name_is(name, "_GLOBAL_OFFSET_TABLE_"))
    {
        *out = lk->dyn_addr[DSEC_GOT_PLT];
        return true;
    }
    if (lk->dynamic && name_is(name, "_DYNAMIC"))
    {
        *out = lk->dyn_addr[DSEC_DYNAMIC];
        return true;
    }
    if (name_is(name, "__bss_start") || name_is(name, "_edata"))
    {
        *out = bss->addr;
        return true;
    }
    if (name_is(name, "_end"))
    {
        *out = bss->addr + bss->size;
        return true;
    }
    if (name_is(name, "_etext"))
    {
        *out = lk->out[OUT_TEXT].addr + lk->out[OUT_TEXT].size;
        return true;
    }
    if (name_is(name, "__init_array_start"))
    {
        *out = lk->out[OUT_INIT_ARRAY].addr;
        return true;
    }
    if (name_is(name, "__init_array_end"))
    {
        *out = lk->out[OUT_INIT_ARRAY].addr + lk->out[OUT_INIT_ARRAY].size;
        return true;
    }
    if (name_is(name, "__fini_array_start"))
    {
        *out = lk->out[OUT_FINI_ARRAY].addr;
        return true;
    }
    if (name_is(name, "__fini_array_end"))
    {
        *out = lk->out[OUT_FINI_ARRAY].addr + lk->out[OUT_FINI_ARRAY].size;
        return true;
    }
    if (name_is(name, "__executable_start") || name_is(name, "__ehdr_start"))
    {
        *out = LINK_BASE;
        return true;
    }
    return false;
}

static bool symbol_value(Linker *lk, InputObject *io, LinkSym *sym, u32 reloc_type, u64 *out)
{
    if (sym->bind == STB_LOCAL)
    {
        if (sym->shndx == SHN_ABS)
        {
            *out = sym->value;
            return true;
        }
        if (sym->shndx >= vec_size(io->obj->sections) || io->sec_out[sym->shndx] == OUT_NULL)
        {
            link_error("%s: bad local symbol '%s'", io->obj->name, sym->name);
            return false;
        }
        OutSec *o = &lk->out[io->sec_out[sym->shndx]];
        if (sym->type == STT_SECTION)
        {
            *out = o->addr + io->sec_off[sym->shndx];
            return true;
        }
        *out = o->addr + io->sec_off[sym->shndx] + sym->value;
        return true;
    }
    GlobalSym *g = strmap_get(lk->globals, sym->name);
    if (g && g->is_defined)
    {
        *out = g->addr;
        return true;
    }
    if (synth_value(lk, sym->name, out))
    {
        return true;
    }
    if (lk->dynamic)
    {
        DsoSym *ds = dso_lookup(lk, sym->name);
        if (ds)
        {
            if (reloc_type == R_X86_64_PLT32 || ds->type == STT_FUNC || ds->type == STT_GNU_IFUNC)
            {
                PltSlot *p = plt_slot(lk, sym->name);
                *out = p->plt_addr;
                return true;
            }
            if (ds->type == STT_OBJECT || ds->type == STT_NOTYPE)
            {
                CopySlot *c = copy_slot(lk, sym->name, ds->size, 8);
                *out = c->addr;
                return true;
            }
            link_error("%s: unsupported dynamic reference to '%s'", io->obj->name, sym->name);
            return false;
        }
    }
    if (sym->bind == STB_WEAK)
    {
        *out = 0;
        return true;
    }
    link_error("%s: undefined reference to '%s'", io->obj->name, sym->name);
    return false;
}

static void finalize_globals(Linker *lk)
{
    for (size_t i = 0; i < vec_size(lk->global_order); i++)
    {
        GlobalSym *g = (GlobalSym *) vec_get(lk->global_order, i);
        if (!g->is_defined)
        {
            continue;
        }
        if (g->is_abs)
        {
            g->addr = g->value;
        }
        else
        {
            g->addr = lk->out[g->out_sec].addr + g->value;
        }
    }
}

static void prepare_entry(Linker *lk)
{
    GlobalSym *start = strmap_get(lk->globals, "_start");
    if (start && start->is_defined)
    {
        return;
    }
    GlobalSym *main_sym = strmap_get(lk->globals, "main");
    if (!main_sym || !main_sym->is_defined)
    {
        link_error("no entry point: '_start' and 'main' are both undefined");
        lk->nerrors++;
        return;
    }
    if (lk->dynamic)
    {
        return; /* a dynamic link takes _start from crt1.o */
    }
    const LinkArch *arch = x86_link_arch();
    OutSec *text = &lk->out[OUT_TEXT];
    lk->start_off = align_up(text->size, 16);
    while (text->bytes.len < lk->start_off)
    {
        bytebuf_append(&text->bytes, 0);
    }
    lk->start_rel_off = arch->emit_start(&text->bytes);
    text->size = text->bytes.len;
    lk->has_start_stub = true;
}

static void place_section(Linker *lk, OutSecId id, u64 *off)
{
    OutSec *o = &lk->out[id];
    *off = align_up(*off, o->align);
    o->offset = *off;
    o->addr = LINK_BASE + *off;
    *off += o->size;
}

static void layout(Linker *lk)
{
    u64 nph = 5;
    u64 off = sizeof(Elf64_Ehdr) + nph * sizeof(Elf64_Phdr);
    lk->out[OUT_TEXT].offset = off;
    lk->out[OUT_TEXT].addr = LINK_BASE + off;
    off += lk->out[OUT_TEXT].size;
    lk->seg_off[0] = 0;
    lk->seg_addr[0] = LINK_BASE;
    lk->seg_filesz[0] = off;
    lk->seg_memsz[0] = off;

    off = align_up(off, LINK_PAGE);
    u64 seg1 = off;
    place_section(lk, OUT_RODATA, &off);
    place_section(lk, OUT_EH_FRAME, &off);
    place_section(lk, OUT_GCC_EXCEPT, &off);
    lk->seg_off[1] = seg1;
    lk->seg_addr[1] = LINK_BASE + seg1;
    lk->seg_filesz[1] = off - seg1;
    lk->seg_memsz[1] = lk->seg_filesz[1];

    off = align_up(off, LINK_PAGE);
    u64 seg2 = off;
    place_section(lk, OUT_DATA, &off);
    place_section(lk, OUT_INIT_ARRAY, &off);
    place_section(lk, OUT_FINI_ARRAY, &off);
    lk->seg_off[2] = seg2;
    lk->seg_addr[2] = LINK_BASE + seg2;
    lk->seg_filesz[2] = off - seg2;
    lk->out[OUT_BSS].addr = LINK_BASE + off;
    lk->out[OUT_BSS].offset = off;
    lk->seg_memsz[2] = lk->seg_filesz[2] + lk->out[OUT_BSS].size;
}

static u32 reloc_width(u32 type)
{
    switch (type)
    {
        case R_X86_64_NONE:
            return 0;
        case R_X86_64_64:
        case R_X86_64_PC64:
            return 8;
        default:
            return 4;
    }
}

static bool apply_relocs(Linker *lk)
{
    const LinkArch *arch = x86_link_arch();
    for (size_t oi = 0; oi < vec_size(lk->objects); oi++)
    {
        InputObject *io = (InputObject *) vec_get(lk->objects, oi);
        u64 nsec = vec_size(io->obj->sections);
        for (u64 si = 0; si < nsec; si++)
        {
            OutSecId id = (OutSecId) io->sec_out[si];
            if (id == OUT_NULL)
            {
                continue;
            }
            OutSec *o = &lk->out[id];
            Vec *list = (Vec *) vec_get(io->obj->relocs, si);
            for (size_t ri = 0; ri < vec_size(list); ri++)
            {
                LinkReloc *r = (LinkReloc *) vec_get(list, ri);
                u64 at = io->sec_off[si] + r->offset;
                if (o->bytes.data == NULL || at + reloc_width(r->type) > o->bytes.len)
                {
                    link_error("%s: relocation out of range", io->obj->name);
                    return false;
                }
                LinkSym *sym = (LinkSym *) vec_get(io->obj->symbols, r->sym);
                u64 place = o->addr + at;
                u8 *field = o->bytes.data + at;
                if (lk->dynamic && is_gotpcrel(r->type))
                {
                    GotSlot *g = got_slot(lk, sym->name);
                    i64 v = (i64) g->addr + r->addend - (i64) place;
                    if (v < INT32_MIN || v > INT32_MAX)
                    {
                        link_error("%s: GOT relocation overflow for '%s'", io->obj->name,
                                   sym->name);
                        return false;
                    }
                    i32 v32 = (i32) v;
                    memcpy(field, &v32, 4);
                    continue;
                }
                u64 s = 0;
                if (!symbol_value(lk, io, sym, r->type, &s))
                {
                    return false;
                }
                RelocResult rr = arch->apply_reloc(r->type, field, s, r->addend, place);
                if (rr == RELOC_OVERFLOW)
                {
                    link_error("%s: relocation overflow for '%s'", io->obj->name, sym->name);
                    return false;
                }
                if (rr == RELOC_UNSUPPORTED)
                {
                    link_error("%s: unsupported relocation type %u", io->obj->name, r->type);
                    return false;
                }
            }
        }
    }
    return true;
}

static bool patch_entry(Linker *lk)
{
    if (!lk->has_start_stub)
    {
        GlobalSym *start = strmap_get(lk->globals, "_start");
        if (start && start->is_defined)
        {
            lk->entry = start->addr;
            return true;
        }
        link_error("no entry point");
        return false;
    }
    GlobalSym *main_sym = strmap_get(lk->globals, "main");
    if (!main_sym || !main_sym->is_defined)
    {
        link_error("undefined reference to 'main'");
        return false;
    }
    const LinkArch *arch = x86_link_arch();
    OutSec *text = &lk->out[OUT_TEXT];
    u64 at = lk->start_off + lk->start_rel_off;
    u64 place = text->addr + at;
    u8 *field = text->bytes.data + at;
    RelocResult rr = arch->apply_reloc(arch->reloc_pc32, field, main_sym->addr, -4, place);
    if (rr != RELOC_OK)
    {
        link_error("entry stub relocation to 'main' failed");
        return false;
    }
    lk->entry = text->addr + lk->start_off;
    GlobalSym *st = strmap_get(lk->globals, "_start");
    if (!st)
    {
        st = global_create(lk, "_start");
    }
    st->is_defined = true;
    st->type = STT_FUNC;
    st->out_sec = OUT_TEXT;
    st->value = lk->start_off;
    st->addr = lk->entry;
    return true;
}

/* Executable section indices (fixed order). */
enum
{
    EXE_NULL = 0,
    EXE_TEXT,
    EXE_RODATA,
    EXE_DATA,
    EXE_BSS,
    EXE_INIT,
    EXE_FINI,
    EXE_EH,
    EXE_GCC,
    EXE_SYMTAB,
    EXE_STRTAB,
    EXE_SHSTRTAB,
    EXE_NSEC
};

static u16 out_sec_index(OutSecId id)
{
    switch (id)
    {
        case OUT_TEXT:
            return EXE_TEXT;
        case OUT_RODATA:
            return EXE_RODATA;
        case OUT_DATA:
            return EXE_DATA;
        case OUT_BSS:
            return EXE_BSS;
        case OUT_INIT_ARRAY:
            return EXE_INIT;
        case OUT_FINI_ARRAY:
            return EXE_FINI;
        case OUT_EH_FRAME:
            return EXE_EH;
        case OUT_GCC_EXCEPT:
            return EXE_GCC;
        default:
            return EXE_NULL;
    }
}

static OutSec *exec_out(Linker *lk, u16 idx)
{
    switch (idx)
    {
        case EXE_TEXT:
            return &lk->out[OUT_TEXT];
        case EXE_RODATA:
            return &lk->out[OUT_RODATA];
        case EXE_DATA:
            return &lk->out[OUT_DATA];
        case EXE_BSS:
            return &lk->out[OUT_BSS];
        case EXE_INIT:
            return &lk->out[OUT_INIT_ARRAY];
        case EXE_FINI:
            return &lk->out[OUT_FINI_ARRAY];
        case EXE_EH:
            return &lk->out[OUT_EH_FRAME];
        case EXE_GCC:
            return &lk->out[OUT_GCC_EXCEPT];
        default:
            return NULL;
    }
}

static void strtab_init(ByteBuf *st, Arena *arena)
{
    bytebuf_init(st, arena);
    bytebuf_append(st, 0);
}

static u32 strtab_add(ByteBuf *st, const char *s)
{
    u32 off = (u32) bytebuf_len(st);
    bytebuf_append_bytes(st, (const u8 *) s, strlen(s));
    bytebuf_append(st, 0);
    return off;
}

static void sym_emit(ByteBuf *symtab, u32 name_off, u8 info, u16 shndx, u64 value, u64 size)
{
    bytebuf_append_u32(symtab, name_off);
    bytebuf_append(symtab, info);
    bytebuf_append(symtab, 0);
    bytebuf_append_u16(symtab, shndx);
    bytebuf_append_u64(symtab, value);
    bytebuf_append_u64(symtab, size);
}

static void phdr_emit(ByteBuf *out, u32 type, u32 flags, u64 offset, u64 vaddr, u64 filesz,
                      u64 memsz, u64 align)
{
    bytebuf_append_u32(out, type);
    bytebuf_append_u32(out, flags);
    bytebuf_append_u64(out, offset);
    bytebuf_append_u64(out, vaddr);
    bytebuf_append_u64(out, vaddr);
    bytebuf_append_u64(out, filesz);
    bytebuf_append_u64(out, memsz);
    bytebuf_append_u64(out, align);
}

static void pad_to(ByteBuf *out, u64 target)
{
    while (bytebuf_len(out) < target)
    {
        bytebuf_append(out, 0);
    }
}

static OutSecId out_sec_id_of(u16 idx)
{
    switch (idx)
    {
        case EXE_TEXT:
            return OUT_TEXT;
        case EXE_RODATA:
            return OUT_RODATA;
        case EXE_DATA:
            return OUT_DATA;
        case EXE_BSS:
            return OUT_BSS;
        case EXE_INIT:
            return OUT_INIT_ARRAY;
        case EXE_FINI:
            return OUT_FINI_ARRAY;
        case EXE_EH:
            return OUT_EH_FRAME;
        case EXE_GCC:
            return OUT_GCC_EXCEPT;
        default:
            return OUT_NULL;
    }
}

static void emit_symtab(Linker *lk, ByteBuf *symtab, ByteBuf *strtab)
{
    for (u64 i = 0; i < sizeof(Elf64_Sym); i++)
    {
        bytebuf_append(symtab, 0);
    }
    for (u16 i = EXE_TEXT; i <= EXE_GCC; i++)
    {
        sym_emit(symtab, 0, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), i, 0, 0);
    }
    for (size_t i = 0; i < vec_size(lk->global_order); i++)
    {
        GlobalSym *g = (GlobalSym *) vec_get(lk->global_order, i);
        if (!g->is_defined)
        {
            continue;
        }
        u8 bind = g->is_weak ? STB_WEAK : STB_GLOBAL;
        u16 shndx = g->is_abs ? SHN_ABS : out_sec_index((OutSecId) g->out_sec);
        sym_emit(symtab, strtab_add(strtab, g->name), ELF64_ST_INFO(bind, g->type), shndx, g->addr,
                 0);
    }
}

static void emit_shdrs(Linker *lk, ByteBuf *out, ByteBuf *shstrtab, u32 names[EXE_NSEC],
                       const u64 off[EXE_NSEC], const u64 size[EXE_NSEC], u32 first_global)
{
    bytebuf_append_u32(out, 0);
    bytebuf_append_u32(out, SHT_NULL);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u32(out, 0);
    bytebuf_append_u32(out, 0);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u64(out, 0);
    for (u16 i = EXE_TEXT; i < EXE_NSEC; i++)
    {
        u32 type;
        u64 flags = 0, align = 1, entsize = 0, addr = 0;
        u32 link = 0, info = 0;
        if (i <= EXE_GCC)
        {
            const OutSecDesc *d = &OUT_SECS[out_sec_id_of(i)];
            OutSec *o = exec_out(lk, i);
            type = d->type;
            flags = d->flags;
            align = o->align ? o->align : 1;
            entsize = d->entsize;
            addr = o->addr;
        }
        else if (i == EXE_SYMTAB)
        {
            type = SHT_SYMTAB;
            link = EXE_STRTAB;
            info = first_global;
            align = 8;
            entsize = sizeof(Elf64_Sym);
        }
        else
        {
            type = SHT_STRTAB;
        }
        bytebuf_append_u32(out, names[i]);
        bytebuf_append_u32(out, type);
        bytebuf_append_u64(out, flags);
        bytebuf_append_u64(out, addr);
        bytebuf_append_u64(out, off[i]);
        bytebuf_append_u64(out, size[i]);
        bytebuf_append_u32(out, link);
        bytebuf_append_u32(out, info);
        bytebuf_append_u64(out, align);
        bytebuf_append_u64(out, entsize);
    }
    (void) shstrtab;
}

static void section_offsets(Linker *lk, u64 off[EXE_NSEC], u64 size[EXE_NSEC])
{
    off[EXE_NULL] = 0;
    size[EXE_NULL] = 0;
    for (u16 i = EXE_TEXT; i <= EXE_GCC; i++)
    {
        OutSec *o = exec_out(lk, i);
        off[i] = o->offset;
        size[i] = o->size;
    }
}

static void append_out_section(ByteBuf *out, Linker *lk, u16 idx, const u64 off[EXE_NSEC])
{
    OutSec *o = exec_out(lk, idx);
    pad_to(out, off[idx]);
    bytebuf_append_bytes(out, o->bytes.data, o->bytes.len);
}

static void emit_ehdr(ByteBuf *out, u64 entry, u64 shoff)
{
    bytebuf_append(out, ELFMAG0);
    bytebuf_append(out, ELFMAG1);
    bytebuf_append(out, ELFMAG2);
    bytebuf_append(out, ELFMAG3);
    bytebuf_append(out, ELFCLASS64);
    bytebuf_append(out, ELFDATA2LSB);
    bytebuf_append(out, EV_CURRENT);
    bytebuf_append(out, 0);
    for (int i = 0; i < 8; i++)
    {
        bytebuf_append(out, 0);
    }
    bytebuf_append_u16(out, ET_EXEC);
    bytebuf_append_u16(out, EM_X86_64);
    bytebuf_append_u32(out, EV_CURRENT);
    bytebuf_append_u64(out, entry);
    bytebuf_append_u64(out, sizeof(Elf64_Ehdr));
    bytebuf_append_u64(out, shoff);
    bytebuf_append_u32(out, 0);
    bytebuf_append_u16(out, sizeof(Elf64_Ehdr));
    bytebuf_append_u16(out, sizeof(Elf64_Phdr));
    bytebuf_append_u16(out, 5);
    bytebuf_append_u16(out, sizeof(Elf64_Shdr));
    bytebuf_append_u16(out, EXE_NSEC);
    bytebuf_append_u16(out, EXE_SHSTRTAB);
}

static void emit_phdrs(Linker *lk, ByteBuf *out)
{
    u64 phdr_off = sizeof(Elf64_Ehdr);
    u64 phdr_size = 5 * sizeof(Elf64_Phdr);
    phdr_emit(out, PT_PHDR, PF_R, phdr_off, LINK_BASE + phdr_off, phdr_size, phdr_size, 8);
    phdr_emit(out, PT_LOAD, PF_R | PF_X, lk->seg_off[0], lk->seg_addr[0], lk->seg_filesz[0],
              lk->seg_memsz[0], LINK_PAGE);
    phdr_emit(out, PT_LOAD, PF_R, lk->seg_off[1], lk->seg_addr[1], lk->seg_filesz[1],
              lk->seg_memsz[1], LINK_PAGE);
    phdr_emit(out, PT_LOAD, PF_R | PF_W, lk->seg_off[2], lk->seg_addr[2], lk->seg_filesz[2],
              lk->seg_memsz[2], LINK_PAGE);
    phdr_emit(out, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0x10);
}

static bool write_executable(Linker *lk)
{
    Arena *arena = lk->arena;
    ByteBuf shstrtab, strtab, symtab;
    strtab_init(&shstrtab, arena);
    u32 names[EXE_NSEC];
    names[EXE_NULL] = 0;
    names[EXE_TEXT] = strtab_add(&shstrtab, ".text");
    names[EXE_RODATA] = strtab_add(&shstrtab, ".rodata");
    names[EXE_DATA] = strtab_add(&shstrtab, ".data");
    names[EXE_BSS] = strtab_add(&shstrtab, ".bss");
    names[EXE_INIT] = strtab_add(&shstrtab, ".init_array");
    names[EXE_FINI] = strtab_add(&shstrtab, ".fini_array");
    names[EXE_EH] = strtab_add(&shstrtab, ".eh_frame");
    names[EXE_GCC] = strtab_add(&shstrtab, ".gcc_except_table");
    names[EXE_SYMTAB] = strtab_add(&shstrtab, ".symtab");
    names[EXE_STRTAB] = strtab_add(&shstrtab, ".strtab");
    names[EXE_SHSTRTAB] = strtab_add(&shstrtab, ".shstrtab");

    strtab_init(&strtab, arena);
    bytebuf_init(&symtab, arena);
    emit_symtab(lk, &symtab, &strtab);
    u32 first_global = EXE_GCC - EXE_TEXT + 2;

    u64 off[EXE_NSEC], size[EXE_NSEC];
    section_offsets(lk, off, size);
    u64 cursor = align_up(lk->seg_off[2] + lk->seg_filesz[2], 8);
    off[EXE_SYMTAB] = cursor;
    size[EXE_SYMTAB] = bytebuf_len(&symtab);
    cursor += size[EXE_SYMTAB];
    off[EXE_STRTAB] = cursor;
    size[EXE_STRTAB] = bytebuf_len(&strtab);
    cursor += size[EXE_STRTAB];
    off[EXE_SHSTRTAB] = cursor;
    size[EXE_SHSTRTAB] = bytebuf_len(&shstrtab);
    cursor += size[EXE_SHSTRTAB];
    u64 shoff = align_up(cursor, 8);

    ByteBuf out;
    bytebuf_init(&out, arena);
    emit_ehdr(&out, lk->entry, shoff);
    emit_phdrs(lk, &out);
    append_out_section(&out, lk, EXE_TEXT, off);
    append_out_section(&out, lk, EXE_RODATA, off);
    append_out_section(&out, lk, EXE_EH, off);
    append_out_section(&out, lk, EXE_GCC, off);
    append_out_section(&out, lk, EXE_DATA, off);
    append_out_section(&out, lk, EXE_INIT, off);
    append_out_section(&out, lk, EXE_FINI, off);
    pad_to(&out, off[EXE_SYMTAB]);
    bytebuf_append_bytes(&out, bytebuf_data(&symtab), bytebuf_len(&symtab));
    bytebuf_append_bytes(&out, bytebuf_data(&strtab), bytebuf_len(&strtab));
    bytebuf_append_bytes(&out, bytebuf_data(&shstrtab), bytebuf_len(&shstrtab));
    pad_to(&out, shoff);
    emit_shdrs(lk, &out, &shstrtab, names, off, size, first_global);

    const char *path = lk->cfg->output_path ? lk->cfg->output_path : "a.out";
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        link_error("%s: cannot open output", path);
        return false;
    }
    fwrite(bytebuf_data(&out), 1, bytebuf_len(&out), f);
    fclose(f);
    chmod(path, 0755);
    return true;
}

static const char *const DEFAULT_LIB_DIRS[] = {
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib64",
    "/usr/lib",
    "/lib/x86_64-linux-gnu",
    "/lib64",
    "/lib",
    NULL,
};

typedef struct
{
    const char *name;
    u64 offset; /* archive file offset of the member header */
    u64 size;   /* member data size */
    bool loaded;
} ArchiveMember;

typedef struct
{
    const char *path;
    const u8 *data;
    size_t len;
    Vec *members;  /* Vec<ArchiveMember*> */
    StrMap *index; /* symbol name -> ArchiveMember* (first definition wins) */
} Archive;

static u32 be32(const u8 *p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | (u32) p[3];
}

static u64 be64(const u8 *p)
{
    u64 hi = be32(p);
    u64 lo = be32(p + 4);
    return (hi << 32) | lo;
}

static u64 parse_dec(const u8 *p, size_t n)
{
    u64 v = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (p[i] < '0' || p[i] > '9')
        {
            break;
        }
        v = v * 10 + (u64) (p[i] - '0');
    }
    return v;
}

static bool is_elf(const u8 *data, size_t len)
{
    return len >= 4 && data[0] == ELFMAG0 && data[1] == ELFMAG1 && data[2] == ELFMAG2 &&
           data[3] == ELFMAG3;
}

static bool is_archive(const u8 *data, size_t len)
{
    return len >= 8 && memcmp(data, "!<arch>\n", 8) == 0;
}

static ArchiveMember *archive_member_at(Archive *ar, u64 offset)
{
    for (size_t i = 0; i < vec_size(ar->members); i++)
    {
        ArchiveMember *m = (ArchiveMember *) vec_get(ar->members, i);
        if (m->offset == offset)
        {
            return m;
        }
    }
    return NULL;
}

static const char *archive_member_name(const u8 *header, const u8 *longnames, size_t longlen,
                                       Arena *arena)
{
    char raw[17];
    memcpy(raw, header, 16);
    raw[16] = '\0';
    for (int i = 15; i >= 0 && raw[i] == ' '; i--)
    {
        raw[i] = '\0';
    }
    if (raw[0] == '/' && raw[1] >= '0' && raw[1] <= '9' && longnames)
    {
        u64 off = parse_dec((const u8 *) raw + 1, strlen(raw) - 1);
        if (off >= longlen)
        {
            return NULL;
        }
        const char *name = (const char *) (longnames + off);
        size_t n = 0;
        while (off + n < longlen && name[n] != '\n' && name[n] != '/' && name[n] != '\0')
        {
            n++;
        }
        char *copy = arena_alloc(arena, n + 1, 1);
        memcpy(copy, name, n);
        copy[n] = '\0';
        return copy;
    }
    size_t n = strlen(raw);
    if (n > 0 && raw[n - 1] == '/')
    {
        raw[n - 1] = '\0';
    }
    char *copy = arena_alloc(arena, strlen(raw) + 1, 1);
    strcpy(copy, raw);
    return copy;
}

static void archive_read_index(Archive *ar, const u8 *idx, size_t len, bool is64, Arena *arena)
{
    if (len < 4)
    {
        return;
    }
    u64 count = is64 ? (len >= 8 ? be64(idx) : 0) : be32(idx);
    size_t base = is64 ? 8 : 4;
    size_t esz = is64 ? 8 : 4;
    if (count > (len - base) / esz)
    {
        return;
    }
    const u8 *names = idx + base + count * esz;
    const u8 *end = idx + len;
    for (u64 i = 0; i < count; i++)
    {
        u64 off = is64 ? be64(idx + base + i * 8) : be32(idx + base + i * 4);
        if (names >= end)
        {
            break;
        }
        const char *sym = (const char *) names;
        while (names < end && *names != '\0')
        {
            names++;
        }
        names++;
        ArchiveMember *m = archive_member_at(ar, off);
        if (m && !strmap_get(ar->index, sym))
        {
            strmap_set(ar->index, sym, m);
        }
    }
    (void) arena;
}

static Archive *archive_parse(const u8 *data, size_t len, const char *path, Arena *arena)
{
    Archive *ar = arena_alloc(arena, sizeof(*ar), sizeof(void *));
    ar->path = path;
    ar->data = data;
    ar->len = len;
    ar->members = vec_new(arena);
    ar->index = strmap_new(arena);

    const u8 *longnames = NULL;
    size_t longlen = 0;
    const u8 *symidx = NULL;
    size_t symlen = 0;
    bool sym64 = false;
    u64 off = 8;
    while (off + 60 <= len)
    {
        const u8 *h = data + off;
        if (h[58] != '`' || h[59] != '\n')
        {
            link_error("%s: malformed archive member header", path);
            return NULL;
        }
        u64 msize = parse_dec(h + 48, 10);
        if (off + 60 + msize > len)
        {
            link_error("%s: archive member extends past end of file", path);
            return NULL;
        }
        char raw[17];
        memcpy(raw, h, 16);
        raw[16] = '\0';
        if (strncmp(raw, "//", 2) == 0 && raw[2] == ' ')
        {
            longnames = h + 60;
            longlen = msize;
        }
        else if (strncmp(raw, "/SYM64/", 7) == 0)
        {
            symidx = h + 60;
            symlen = msize;
            sym64 = true;
        }
        else if (raw[0] == '/' && raw[1] == ' ')
        {
            symidx = h + 60;
            symlen = msize;
            sym64 = false;
        }
        else
        {
            ArchiveMember *m = arena_alloc(arena, sizeof(*m), sizeof(void *));
            m->name = archive_member_name(h, longnames, longlen, arena);
            m->offset = off;
            m->size = msize;
            m->loaded = false;
            if (!m->name)
            {
                link_error("%s: bad archive member name", path);
                return NULL;
            }
            vec_push(ar->members, m);
        }
        off += 60 + msize;
        if (msize & 1)
        {
            off++;
        }
    }
    if (symidx)
    {
        archive_read_index(ar, symidx, symlen, sym64, arena);
    }
    return ar;
}

static ArchiveMember *archive_find(Archive *ar, const char *name)
{
    return (ArchiveMember *) strmap_get(ar->index, name);
}

static bool link_add_path(Linker *lk, const char *path);

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return false;
    }
    fclose(f);
    return true;
}

static const char *path_join(const char *dir, const char *name, Arena *arena)
{
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = arena_alloc(arena, n, 1);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static const char *resolve_library(Linker *lk, const char *name)
{
    char fname[256];
    for (size_t i = 0; i < vec_size(lk->search_dirs); i++)
    {
        const char *dir = (const char *) vec_get(lk->search_dirs, i);
        snprintf(fname, sizeof(fname), "lib%s.so", name);
        const char *cand = path_join(dir, fname, lk->arena);
        if (file_exists(cand))
        {
            return cand;
        }
        snprintf(fname, sizeof(fname), "lib%s.a", name);
        cand = path_join(dir, fname, lk->arena);
        if (file_exists(cand))
        {
            return cand;
        }
    }
    link_error("cannot find -l%s", name);
    return NULL;
}

static const char *find_in_dirs(Linker *lk, const char *name)
{
    for (size_t i = 0; i < vec_size(lk->search_dirs); i++)
    {
        const char *cand = path_join((const char *) vec_get(lk->search_dirs, i), name, lk->arena);
        if (file_exists(cand))
        {
            return cand;
        }
    }
    return NULL;
}

typedef struct
{
    Vec *tokens; /* Vec<const char*> */
    size_t i;
} ScriptScan;

static const char *script_peek(ScriptScan *s)
{
    if (s->i >= vec_size(s->tokens))
    {
        return NULL;
    }
    return (const char *) vec_get(s->tokens, s->i);
}

static void script_tokenize(const u8 *data, size_t len, Vec *tokens, Arena *arena)
{
    size_t i = 0;
    while (i < len)
    {
        if (data[i] == '/' && i + 1 < len && data[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < len && !(data[i] == '*' && data[i + 1] == '/'))
            {
                i++;
            }
            i += 2;
            continue;
        }
        if (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')
        {
            i++;
            continue;
        }
        if (data[i] == '(' || data[i] == ')' || data[i] == ',')
        {
            char *tok = arena_alloc(arena, 2, 1);
            tok[0] = (char) data[i];
            tok[1] = '\0';
            vec_push(tokens, tok);
            i++;
            continue;
        }
        size_t start = i;
        while (i < len && data[i] != ' ' && data[i] != '\t' && data[i] != '\n' && data[i] != '\r' &&
               data[i] != '(' && data[i] != ')' && data[i] != ',')
        {
            i++;
        }
        size_t n = i - start;
        char *tok = arena_alloc(arena, n + 1, 1);
        memcpy(tok, data + start, n);
        tok[n] = '\0';
        vec_push(tokens, tok);
    }
}

static bool script_directive(const char *word)
{
    if (!(word[0] >= 'A' && word[0] <= 'Z'))
    {
        return false;
    }
    for (const char *p = word; *p; p++)
    {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
        {
            return false;
        }
    }
    return true;
}

static void script_skip_parens(ScriptScan *s)
{
    if (script_peek(s) && strcmp(script_peek(s), "(") == 0)
    {
        s->i++;
    }
    int depth = 1;
    while (depth > 0)
    {
        const char *t = script_peek(s);
        if (!t)
        {
            return;
        }
        if (strcmp(t, "(") == 0)
        {
            depth++;
        }
        else if (strcmp(t, ")") == 0)
        {
            depth--;
        }
        s->i++;
    }
}

static bool script_parse_list(Linker *lk, ScriptScan *s, const char *base_dir, int depth);

static bool script_add_file(Linker *lk, const char *word, const char *base_dir)
{
    if (word[0] == '/')
    {
        return link_add_path(lk, word);
    }
    if (base_dir)
    {
        const char *cand = path_join(base_dir, word, lk->arena);
        if (file_exists(cand))
        {
            return link_add_path(lk, cand);
        }
    }
    for (size_t i = 0; i < vec_size(lk->search_dirs); i++)
    {
        const char *cand = path_join((const char *) vec_get(lk->search_dirs, i), word, lk->arena);
        if (file_exists(cand))
        {
            return link_add_path(lk, cand);
        }
    }
    link_error("ld script: cannot find '%s'", word);
    return false;
}

static bool script_parse_list(Linker *lk, ScriptScan *s, const char *base_dir, int depth)
{
    if (depth > 16)
    {
        link_error("ld script nesting too deep");
        return false;
    }
    while (s->i < vec_size(s->tokens))
    {
        const char *t = script_peek(s);
        if (strcmp(t, ")") == 0)
        {
            s->i++;
            return true;
        }
        if (strcmp(t, "(") == 0 || strcmp(t, ",") == 0)
        {
            s->i++;
            continue;
        }
        if (script_directive(t))
        {
            const char *dir = t;
            s->i++;
            if (strcmp(dir, "OUTPUT_FORMAT") == 0 || strcmp(dir, "OUTPUT_ARCH") == 0 ||
                strcmp(dir, "TARGET") == 0 || strcmp(dir, "ENTRY") == 0)
            {
                script_skip_parens(s);
            }
            else if (strcmp(dir, "SEARCH_DIR") == 0)
            {
                s->i++;
                const char *d = script_peek(s);
                if (d)
                {
                    vec_push(lk->search_dirs, (void *) d);
                    s->i++;
                }
                script_skip_parens(s);
            }
            else if (strcmp(dir, "GROUP") == 0 || strcmp(dir, "INPUT") == 0 ||
                     strcmp(dir, "AS_NEEDED") == 0)
            {
                if (script_peek(s) && strcmp(script_peek(s), "(") == 0)
                {
                    s->i++;
                }
                if (!script_parse_list(lk, s, base_dir, depth + 1))
                {
                    return false;
                }
            }
            else
            {
                link_error("ld script: unsupported directive '%s'", dir);
                return false;
            }
        }
        else
        {
            s->i++;
            if (!script_add_file(lk, t, base_dir))
            {
                return false;
            }
        }
    }
    return true;
}

static const char *dir_of(const char *path, Arena *arena)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
    {
        return NULL;
    }
    size_t n = (size_t) (slash - path);
    char *d = arena_alloc(arena, n + 1, 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static Dso *dso_read(const u8 *data, size_t len, const char *path, Arena *arena)
{
    if (len < sizeof(Elf64_Ehdr))
    {
        link_error("%s: truncated shared object", path);
        return NULL;
    }
    Elf64_Ehdr eh;
    memcpy(&eh, data, sizeof(eh));
    if (eh.e_shnum == 0)
    {
        link_error("%s: shared object has no section headers", path);
        return NULL;
    }
    const Elf64_Shdr *dynsym = NULL;
    const Elf64_Shdr *dynamic = NULL;
    for (u64 i = 0; i < eh.e_shnum; i++)
    {
        const Elf64_Shdr *sh = shdr_at(data, len, &eh, i);
        if (!sh)
        {
            continue;
        }
        if (sh->sh_type == SHT_DYNSYM)
        {
            dynsym = sh;
        }
        else if (sh->sh_type == SHT_DYNAMIC)
        {
            dynamic = sh;
        }
    }
    if (!dynsym || dynsym->sh_link >= eh.e_shnum)
    {
        link_error("%s: shared object has no dynamic symbol table", path);
        return NULL;
    }
    const Elf64_Shdr *str = shdr_at(data, len, &eh, dynsym->sh_link);
    if (!str || !in_range(len, str->sh_offset, str->sh_size))
    {
        link_error("%s: malformed dynamic string table", path);
        return NULL;
    }
    const u8 *strtab = data + str->sh_offset;
    size_t strtab_len = (size_t) str->sh_size;

    Dso *d = arena_alloc(arena, sizeof(*d), sizeof(void *));
    d->path = path;
    d->soname = NULL;
    d->data = data;
    d->len = len;
    d->exports = strmap_new(arena);

    if (dynamic && dynamic->sh_link == dynsym->sh_link)
    {
        u64 n = dynamic->sh_size / sizeof(Elf64_Dyn);
        for (u64 i = 0; i < n; i++)
        {
            const u8 *p = data + dynamic->sh_offset + i * sizeof(Elf64_Dyn);
            Elf64_Dyn ed;
            memcpy(&ed, p, sizeof(ed));
            if (ed.d_tag == DT_SONAME)
            {
                d->soname = cstr_at(strtab, strtab_len, ed.d_un.d_val);
            }
            else if (ed.d_tag == DT_NULL)
            {
                break;
            }
        }
    }

    u64 entsize = dynsym->sh_entsize ? dynsym->sh_entsize : sizeof(Elf64_Sym);
    u64 nsyms = dynsym->sh_size / entsize;
    for (u64 s = 0; s < nsyms; s++)
    {
        const u8 *p = data + dynsym->sh_offset + s * entsize;
        Elf64_Sym es;
        memcpy(&es, p, sizeof(es));
        u8 bind = ELF64_ST_BIND(es.st_info);
        if (bind == STB_LOCAL || es.st_shndx == SHN_UNDEF)
        {
            continue;
        }
        const char *name = cstr_at(strtab, strtab_len, es.st_name);
        if (!name || strmap_get(d->exports, name))
        {
            continue;
        }
        DsoSym *ds = arena_alloc(arena, sizeof(*ds), sizeof(void *));
        ds->name = name;
        ds->type = ELF64_ST_TYPE(es.st_info);
        ds->bind = bind;
        ds->value = es.st_value;
        ds->size = es.st_size;
        ds->shndx = es.st_shndx;
        strmap_set(d->exports, name, ds);
    }
    return d;
}

static bool link_add_path(Linker *lk, const char *path)
{
    if (strmap_get(lk->loaded_paths, path))
    {
        return true;
    }
    strmap_set(lk->loaded_paths, path, (void *) 1);
    size_t len = 0;
    u8 *data = read_whole_file(path, &len, lk->arena);
    if (!data)
    {
        return false;
    }
    if (is_archive(data, len))
    {
        Archive *ar = archive_parse(data, len, path, lk->arena);
        if (!ar)
        {
            return false;
        }
        vec_push(lk->archives, ar);
        return true;
    }
    if (is_elf(data, len))
    {
        if (len < sizeof(Elf64_Ehdr))
        {
            link_error("%s: truncated ELF file", path);
            return false;
        }
        Elf64_Ehdr eh;
        memcpy(&eh, data, sizeof(eh));
        if (eh.e_type == ET_DYN)
        {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (strncmp(base, "ld-linux", 8) == 0 || strncmp(base, "ld.so", 5) == 0)
            {
                return true; /* the interpreter: not a NEEDED input */
            }
            Dso *d = dso_read(data, len, path, lk->arena);
            if (!d)
            {
                return false;
            }
            vec_push(lk->dsos, d);
            lk->dynamic = true;
            if (d->soname)
            {
                bool seen = false;
                for (size_t k = 0; k < vec_size(lk->needed); k++)
                {
                    if (strcmp((const char *) vec_get(lk->needed, k), d->soname) == 0)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    vec_push(lk->needed, (void *) d->soname);
                }
            }
            return true;
        }
        LinkObject *obj = link_read_memory(data, len, path, lk->arena);
        if (!obj)
        {
            return false;
        }
        return merge_object(lk, obj);
    }
    ScriptScan s = {.tokens = vec_new(lk->arena), .i = 0};
    script_tokenize(data, len, s.tokens, lk->arena);
    if (vec_size(s.tokens) == 0)
    {
        link_error("%s: unrecognized input format", path);
        return false;
    }
    return script_parse_list(lk, &s, dir_of(path, lk->arena), 0);
}

static void collect_undefined(Linker *lk, Vec *out)
{
    for (size_t oi = 0; oi < vec_size(lk->objects); oi++)
    {
        InputObject *io = (InputObject *) vec_get(lk->objects, oi);
        for (u64 si = 0; si < vec_size(io->obj->sections); si++)
        {
            Vec *list = (Vec *) vec_get(io->obj->relocs, si);
            for (size_t ri = 0; ri < vec_size(list); ri++)
            {
                LinkReloc *r = (LinkReloc *) vec_get(list, ri);
                LinkSym *sym = (LinkSym *) vec_get(io->obj->symbols, r->sym);
                if (sym->bind == STB_LOCAL || sym->shndx != SHN_UNDEF)
                {
                    continue;
                }
                if (strmap_get(lk->globals, sym->name))
                {
                    continue;
                }
                vec_push(out, (void *) sym->name);
            }
        }
    }
}

static bool pull_from_archives(Linker *lk)
{
    if (vec_size(lk->archives) == 0)
    {
        return false;
    }
    Vec *undef = vec_new(lk->arena);
    collect_undefined(lk, undef);
    bool pulled = false;
    for (size_t ai = 0; ai < vec_size(lk->archives); ai++)
    {
        Archive *ar = (Archive *) vec_get(lk->archives, ai);
        for (size_t ui = 0; ui < vec_size(undef); ui++)
        {
            const char *name = (const char *) vec_get(undef, ui);
            if (strmap_get(lk->globals, name))
            {
                continue;
            }
            ArchiveMember *m = archive_find(ar, name);
            if (!m || m->loaded)
            {
                continue;
            }
            m->loaded = true;
            char label[512];
            snprintf(label, sizeof(label), "%s(%s)", ar->path, m->name);
            LinkObject *obj =
                link_read_memory(ar->data + m->offset + 60, m->size, label, lk->arena);
            if (!obj || !merge_object(lk, obj))
            {
                return false;
            }
            pulled = true;
        }
    }
    return pulled;
}

static void dyn_emit(ByteBuf *b, i64 tag, u64 val)
{
    bytebuf_append_u64(b, (u64) tag);
    bytebuf_append_u64(b, val);
}

static void dynamic_scan(Linker *lk)
{
    if (!lk->dynamic)
    {
        return;
    }
    DynSym *nullsym = arena_alloc(lk->arena, sizeof(*nullsym), sizeof(void *));
    nullsym->name = "";
    nullsym->info = 0;
    nullsym->shndx = 0;
    nullsym->value = 0;
    nullsym->size = 0;
    nullsym->name_off = 0;
    vec_push(lk->dynsyms, nullsym);

    for (size_t oi = 0; oi < vec_size(lk->objects); oi++)
    {
        InputObject *io = (InputObject *) vec_get(lk->objects, oi);
        for (u64 si = 0; si < vec_size(io->obj->sections); si++)
        {
            Vec *list = (Vec *) vec_get(io->obj->relocs, si);
            for (size_t ri = 0; ri < vec_size(list); ri++)
            {
                LinkReloc *r = (LinkReloc *) vec_get(list, ri);
                LinkSym *sym = (LinkSym *) vec_get(io->obj->symbols, r->sym);
                if (is_gotpcrel(r->type))
                {
                    GotSlot *g = got_slot(lk, sym->name);
                    GlobalSym *gs =
                        sym->bind != STB_LOCAL ? strmap_get(lk->globals, sym->name) : NULL;
                    if (gs && gs->is_defined)
                    {
                        g->is_dynamic = false;
                        continue;
                    }
                    DsoSym *ds = dso_lookup(lk, sym->name);
                    if (!ds)
                    {
                        continue;
                    }
                    if (!g->is_dynamic)
                    {
                        g->is_dynamic = true;
                        u32 idx = dynsym_add(lk, sym->name, ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE),
                                             SHN_UNDEF, 0, 0);
                        DynReloc *dr = arena_alloc(lk->arena, sizeof(*dr), sizeof(void *));
                        dr->offset = 0;
                        dr->sym = idx;
                        dr->type = R_X86_64_GLOB_DAT;
                        dr->addend = 0;
                        dr->owner = g;
                        vec_push(lk->rela_dyn, dr);
                    }
                    continue;
                }
                if (sym->bind == STB_LOCAL || sym->shndx != SHN_UNDEF)
                {
                    continue;
                }
                if (strmap_get(lk->globals, sym->name))
                {
                    continue;
                }
                DsoSym *ds = dso_lookup(lk, sym->name);
                if (!ds)
                {
                    continue;
                }
                if (r->type == R_X86_64_PLT32)
                {
                    plt_slot(lk, sym->name);
                }
                else if (r->type == R_X86_64_32S || r->type == R_X86_64_32 ||
                         r->type == R_X86_64_64 || r->type == R_X86_64_PC32 ||
                         r->type == R_X86_64_PC64)
                {
                    if (ds->type == STT_OBJECT || ds->type == STT_NOTYPE)
                    {
                        CopySlot *c = copy_slot(lk, sym->name, ds->size, 8);
                        if (!c->has_reloc)
                        {
                            c->has_reloc = true;
                            DynReloc *dr = arena_alloc(lk->arena, sizeof(*dr), sizeof(void *));
                            dr->offset = 0;
                            dr->sym = c->dynidx;
                            dr->type = R_X86_64_COPY;
                            dr->addend = 0;
                            dr->owner = c;
                            vec_push(lk->rela_dyn, dr);
                        }
                    }
                    else
                    {
                        link_error("%s: cannot reference dynamic function '%s' by address",
                                   io->obj->name, sym->name);
                        lk->nerrors++;
                    }
                }
            }
        }
    }
}

static void dynamic_build(Linker *lk)
{
    const char *interp = "/lib64/ld-linux-x86-64.so.2";
    bytebuf_append_bytes(&lk->dynbuf[DSEC_INTERP], (const u8 *) interp, strlen(interp) + 1);

    ByteBuf *str = &lk->dynbuf[DSEC_DYNSTR];
    strtab_init(str, lk->arena);
    for (size_t i = 1; i < vec_size(lk->dynsyms); i++)
    {
        DynSym *d = (DynSym *) vec_get(lk->dynsyms, i);
        d->name_off = strtab_add(str, d->name);
    }
    for (size_t i = 0; i < vec_size(lk->needed); i++)
    {
        const char *s = (const char *) vec_get(lk->needed, i);
        u32 off = strtab_add(str, s);
        vec_push(lk->needed_off, (void *) (uintptr_t) off);
    }

    ByteBuf *sym = &lk->dynbuf[DSEC_DYNSYM];
    bytebuf_init(sym, lk->arena);
    for (u64 i = 0; i < vec_size(lk->dynsyms) * sizeof(Elf64_Sym); i++)
    {
        bytebuf_append(sym, 0);
    }

    ByteBuf *hash = &lk->dynbuf[DSEC_HASH];
    bytebuf_init(hash, lk->arena);
    bytebuf_append_u32(hash, 1);
    bytebuf_append_u32(hash, (u32) vec_size(lk->dynsyms));
    bytebuf_append_u32(hash, 1);
    bytebuf_append_u32(hash, 6);
    bytebuf_append_u64(hash, 0);
    bytebuf_append_u32(hash, 0);

    ByteBuf *plt = &lk->dynbuf[DSEC_PLT];
    bytebuf_init(plt, lk->arena);
    for (u64 i = 0; i < vec_size(lk->plt_slots) * 16; i++)
    {
        bytebuf_append(plt, 0);
    }
    ByteBuf *got = &lk->dynbuf[DSEC_GOT];
    bytebuf_init(got, lk->arena);
    for (u64 i = 0; i < vec_size(lk->got_slots) * 8; i++)
    {
        bytebuf_append(got, 0);
    }
    lk->got_plt_size = (3 + vec_size(lk->plt_slots)) * 8;
    ByteBuf *gotplt = &lk->dynbuf[DSEC_GOT_PLT];
    bytebuf_init(gotplt, lk->arena);
    for (u64 i = 0; i < lk->got_plt_size; i++)
    {
        bytebuf_append(gotplt, 0);
    }
    ByteBuf *rd = &lk->dynbuf[DSEC_RELA_DYN];
    bytebuf_init(rd, lk->arena);
    for (u64 i = 0; i < vec_size(lk->rela_dyn) * sizeof(Elf64_Rela); i++)
    {
        bytebuf_append(rd, 0);
    }
    ByteBuf *rp = &lk->dynbuf[DSEC_RELA_PLT];
    bytebuf_init(rp, lk->arena);
    for (u64 i = 0; i < vec_size(lk->rela_plt) * sizeof(Elf64_Rela); i++)
    {
        bytebuf_append(rp, 0);
    }
    u64 ndyn = 16 + vec_size(lk->needed);
    ByteBuf *dyn = &lk->dynbuf[DSEC_DYNAMIC];
    bytebuf_init(dyn, lk->arena);
    for (u64 i = 0; i < ndyn * sizeof(Elf64_Dyn); i++)
    {
        bytebuf_append(dyn, 0);
    }
}

static u64 place_dyn(Linker *lk, DynSecId id, u64 off)
{
    off = align_up(off, DYN_SECS[id].align);
    lk->dyn_off[id] = off;
    lk->dyn_addr[id] = LINK_BASE + off;
    return off + bytebuf_len(&lk->dynbuf[id]);
}

static void layout_dynamic(Linker *lk)
{
    u64 nph = 8;
    u64 off = sizeof(Elf64_Ehdr) + nph * sizeof(Elf64_Phdr);
    for (int id = 0; id <= DSEC_RELA_PLT; id++)
    {
        off = place_dyn(lk, (DynSecId) id, off);
    }
    lk->seg_off[0] = 0;
    lk->seg_addr[0] = LINK_BASE;
    lk->seg_filesz[0] = off;
    lk->seg_memsz[0] = off;

    off = align_up(off, LINK_PAGE);
    u64 s1 = off;
    off = place_dyn(lk, DSEC_PLT, off);
    place_section(lk, OUT_TEXT, &off);
    lk->seg_off[1] = s1;
    lk->seg_addr[1] = LINK_BASE + s1;
    lk->seg_filesz[1] = off - s1;
    lk->seg_memsz[1] = lk->seg_filesz[1];

    off = align_up(off, LINK_PAGE);
    u64 s2 = off;
    place_section(lk, OUT_RODATA, &off);
    place_section(lk, OUT_EH_FRAME, &off);
    place_section(lk, OUT_GCC_EXCEPT, &off);
    lk->seg_off[2] = s2;
    lk->seg_addr[2] = LINK_BASE + s2;
    lk->seg_filesz[2] = off - s2;
    lk->seg_memsz[2] = lk->seg_filesz[2];

    off = align_up(off, LINK_PAGE);
    u64 s3 = off;
    place_section(lk, OUT_INIT_ARRAY, &off);
    place_section(lk, OUT_FINI_ARRAY, &off);
    off = place_dyn(lk, DSEC_DYNAMIC, off);
    off = place_dyn(lk, DSEC_GOT, off);
    off = place_dyn(lk, DSEC_GOT_PLT, off);
    place_section(lk, OUT_DATA, &off);
    lk->out[OUT_BSS].addr = LINK_BASE + off;
    lk->out[OUT_BSS].offset = off;
    lk->seg_off[3] = s3;
    lk->seg_addr[3] = LINK_BASE + s3;
    lk->seg_filesz[3] = off - s3;
    lk->seg_memsz[3] = lk->seg_filesz[3] + lk->out[OUT_BSS].size;
}

/* Dynamic executable section indices (fixed order). */
enum
{
    DX_NULL = 0,
    DX_INTERP,
    DX_HASH,
    DX_DYNSYM,
    DX_DYNSTR,
    DX_RELA_DYN,
    DX_RELA_PLT,
    DX_PLT,
    DX_TEXT,
    DX_RODATA,
    DX_EH,
    DX_INIT,
    DX_FINI,
    DX_DYNAMIC,
    DX_GOT,
    DX_GOT_PLT,
    DX_DATA,
    DX_BSS,
    DX_SYMTAB,
    DX_STRTAB,
    DX_SHSTRTAB,
    DX_NSEC
};

static void poke_u64(ByteBuf *b, u64 off, u64 val)
{
    memcpy(b->data + off, &val, 8);
}

static void poke_u32(ByteBuf *b, u64 off, u32 val)
{
    memcpy(b->data + off, &val, 4);
}

static void finalize_dynamic(Linker *lk)
{
    ByteBuf *gotb = &lk->dynbuf[DSEC_GOT];
    for (size_t i = 0; i < vec_size(lk->got_slots); i++)
    {
        GotSlot *g = (GotSlot *) vec_get(lk->got_slots, i);
        g->addr = lk->dyn_addr[DSEC_GOT] + g->off;
        if (!g->is_dynamic)
        {
            GlobalSym *gs = (GlobalSym *) strmap_get(lk->globals, g->name);
            if (gs && gs->is_defined)
            {
                poke_u64(gotb, g->off, gs->addr);
            }
        }
    }

    ByteBuf *gotplt = &lk->dynbuf[DSEC_GOT_PLT];
    poke_u64(gotplt, 0, lk->dyn_addr[DSEC_DYNAMIC]);

    ByteBuf *pltb = &lk->dynbuf[DSEC_PLT];
    for (size_t i = 0; i < vec_size(lk->plt_slots); i++)
    {
        PltSlot *p = (PltSlot *) vec_get(lk->plt_slots, i);
        p->plt_addr = lk->dyn_addr[DSEC_PLT] + p->plt_off;
        p->got_addr = lk->dyn_addr[DSEC_GOT_PLT] + p->got_off;
        u64 off = p->plt_off;
        pltb->data[off + 0] = 0xff;
        pltb->data[off + 1] = 0x25;
        i64 disp = (i64) p->got_addr - (i64) (p->plt_addr + 6);
        i32 d32 = (i32) disp;
        memcpy(pltb->data + off + 2, &d32, 4);
    }

    for (size_t i = 0; i < vec_size(lk->copies); i++)
    {
        CopySlot *c = (CopySlot *) vec_get(lk->copies, i);
        c->addr = lk->out[OUT_BSS].addr + c->off;
        DynSym *d = (DynSym *) vec_get(lk->dynsyms, c->dynidx);
        d->shndx = DX_BSS;
        d->value = c->addr;
    }

    ByteBuf *rd = &lk->dynbuf[DSEC_RELA_DYN];
    for (size_t i = 0; i < vec_size(lk->rela_dyn); i++)
    {
        DynReloc *r = (DynReloc *) vec_get(lk->rela_dyn, i);
        u64 off = i * sizeof(Elf64_Rela);
        u64 target = 0;
        if (r->type == R_X86_64_GLOB_DAT)
        {
            GotSlot *g = (GotSlot *) r->owner;
            target = g->addr;
        }
        else if (r->type == R_X86_64_COPY)
        {
            CopySlot *c = (CopySlot *) r->owner;
            target = c->addr;
        }
        poke_u64(rd, off, target);
        poke_u64(rd, off + 8, ELF64_R_INFO(r->sym, r->type));
        poke_u64(rd, off + 16, (u64) r->addend);
    }

    ByteBuf *rp = &lk->dynbuf[DSEC_RELA_PLT];
    for (size_t i = 0; i < vec_size(lk->plt_slots); i++)
    {
        PltSlot *p = (PltSlot *) vec_get(lk->plt_slots, i);
        u64 off = i * sizeof(Elf64_Rela);
        poke_u64(rp, off, p->got_addr);
        poke_u64(rp, off + 8, ELF64_R_INFO(p->dynidx, R_X86_64_JUMP_SLOT));
        poke_u64(rp, off + 16, 0);
    }

    ByteBuf *sym = &lk->dynbuf[DSEC_DYNSYM];
    for (size_t i = 1; i < vec_size(lk->dynsyms); i++)
    {
        DynSym *d = (DynSym *) vec_get(lk->dynsyms, i);
        u64 off = i * sizeof(Elf64_Sym);
        poke_u32(sym, off, d->name_off);
        sym->data[off + 4] = d->info;
        sym->data[off + 5] = 0;
        memcpy(sym->data + off + 6, &d->shndx, 2);
        poke_u64(sym, off + 8, d->value);
        poke_u64(sym, off + 16, d->size);
    }

    ByteBuf *dyn = &lk->dynbuf[DSEC_DYNAMIC];
    bytebuf_init(dyn, lk->arena);
    for (size_t i = 0; i < vec_size(lk->needed); i++)
    {
        u64 off = (u64) (uintptr_t) vec_get(lk->needed_off, i);
        dyn_emit(dyn, DT_NEEDED, off);
    }
    dyn_emit(dyn, DT_GNU_HASH, lk->dyn_addr[DSEC_HASH]);
    dyn_emit(dyn, DT_STRTAB, lk->dyn_addr[DSEC_DYNSTR]);
    dyn_emit(dyn, DT_SYMTAB, lk->dyn_addr[DSEC_DYNSYM]);
    dyn_emit(dyn, DT_STRSZ, bytebuf_len(&lk->dynbuf[DSEC_DYNSTR]));
    dyn_emit(dyn, DT_SYMENT, sizeof(Elf64_Sym));
    dyn_emit(dyn, DT_PLTGOT, lk->dyn_addr[DSEC_GOT_PLT]);
    dyn_emit(dyn, DT_PLTRELSZ, bytebuf_len(&lk->dynbuf[DSEC_RELA_PLT]));
    dyn_emit(dyn, DT_PLTREL, DT_RELA);
    dyn_emit(dyn, DT_JMPREL, lk->dyn_addr[DSEC_RELA_PLT]);
    dyn_emit(dyn, DT_RELA, lk->dyn_addr[DSEC_RELA_DYN]);
    dyn_emit(dyn, DT_RELASZ, bytebuf_len(&lk->dynbuf[DSEC_RELA_DYN]));
    dyn_emit(dyn, DT_RELAENT, sizeof(Elf64_Rela));
    dyn_emit(dyn, DT_FLAGS, 0x8);
    dyn_emit(dyn, DT_FLAGS_1, 0x1);
    dyn_emit(dyn, DT_DEBUG, 0);
    dyn_emit(dyn, DT_NULL, 0);
}

static void dyn_shdr(ByteBuf *out, u32 name, u32 type, u64 flags, u64 addr, u64 off, u64 size,
                     u32 link, u32 info, u64 align, u64 entsize)
{
    bytebuf_append_u32(out, name);
    bytebuf_append_u32(out, type);
    bytebuf_append_u64(out, flags);
    bytebuf_append_u64(out, addr);
    bytebuf_append_u64(out, off);
    bytebuf_append_u64(out, size);
    bytebuf_append_u32(out, link);
    bytebuf_append_u32(out, info);
    bytebuf_append_u64(out, align);
    bytebuf_append_u64(out, entsize);
}

static void append_at(ByteBuf *out, u64 off, ByteBuf *src)
{
    pad_to(out, off);
    bytebuf_append_bytes(out, bytebuf_data(src), bytebuf_len(src));
}

static void dyn_ehdr(ByteBuf *out, u64 entry, u64 shoff)
{
    bytebuf_append(out, ELFMAG0);
    bytebuf_append(out, ELFMAG1);
    bytebuf_append(out, ELFMAG2);
    bytebuf_append(out, ELFMAG3);
    bytebuf_append(out, ELFCLASS64);
    bytebuf_append(out, ELFDATA2LSB);
    bytebuf_append(out, EV_CURRENT);
    bytebuf_append(out, 0);
    for (int i = 0; i < 8; i++)
    {
        bytebuf_append(out, 0);
    }
    bytebuf_append_u16(out, ET_EXEC);
    bytebuf_append_u16(out, EM_X86_64);
    bytebuf_append_u32(out, EV_CURRENT);
    bytebuf_append_u64(out, entry);
    bytebuf_append_u64(out, sizeof(Elf64_Ehdr));
    bytebuf_append_u64(out, shoff);
    bytebuf_append_u32(out, 0);
    bytebuf_append_u16(out, sizeof(Elf64_Ehdr));
    bytebuf_append_u16(out, sizeof(Elf64_Phdr));
    bytebuf_append_u16(out, 8);
    bytebuf_append_u16(out, sizeof(Elf64_Shdr));
    bytebuf_append_u16(out, DX_NSEC);
    bytebuf_append_u16(out, DX_SHSTRTAB);
}

static void dyn_phdrs(Linker *lk, ByteBuf *out)
{
    u64 phdr_off = sizeof(Elf64_Ehdr);
    u64 phdr_size = 8 * sizeof(Elf64_Phdr);
    phdr_emit(out, PT_PHDR, PF_R, phdr_off, LINK_BASE + phdr_off, phdr_size, phdr_size, 8);
    phdr_emit(out, PT_INTERP, PF_R, lk->dyn_off[DSEC_INTERP], lk->dyn_addr[DSEC_INTERP],
              bytebuf_len(&lk->dynbuf[DSEC_INTERP]), bytebuf_len(&lk->dynbuf[DSEC_INTERP]), 1);
    phdr_emit(out, PT_LOAD, PF_R, lk->seg_off[0], lk->seg_addr[0], lk->seg_filesz[0],
              lk->seg_memsz[0], LINK_PAGE);
    phdr_emit(out, PT_LOAD, PF_R | PF_X, lk->seg_off[1], lk->seg_addr[1], lk->seg_filesz[1],
              lk->seg_memsz[1], LINK_PAGE);
    phdr_emit(out, PT_LOAD, PF_R, lk->seg_off[2], lk->seg_addr[2], lk->seg_filesz[2],
              lk->seg_memsz[2], LINK_PAGE);
    phdr_emit(out, PT_LOAD, PF_R | PF_W, lk->seg_off[3], lk->seg_addr[3], lk->seg_filesz[3],
              lk->seg_memsz[3], LINK_PAGE);
    phdr_emit(out, PT_DYNAMIC, PF_R | PF_W, lk->dyn_off[DSEC_DYNAMIC], lk->dyn_addr[DSEC_DYNAMIC],
              bytebuf_len(&lk->dynbuf[DSEC_DYNAMIC]), bytebuf_len(&lk->dynbuf[DSEC_DYNAMIC]), 8);
    phdr_emit(out, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0x10);
}

static void dyn_section_headers(Linker *lk, ByteBuf *out, u32 nm[DX_NSEC], u64 off_symtab,
                                u64 size_symtab, u64 off_strtab, u64 size_strtab, u64 off_sh,
                                u64 size_sh)
{
    dyn_shdr(out, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);
    dyn_shdr(out, nm[DX_INTERP], SHT_PROGBITS, SHF_ALLOC, lk->dyn_addr[DSEC_INTERP],
             lk->dyn_off[DSEC_INTERP], bytebuf_len(&lk->dynbuf[DSEC_INTERP]), 0, 0, 1, 0);
    dyn_shdr(out, nm[DX_HASH], SHT_GNU_HASH, SHF_ALLOC, lk->dyn_addr[DSEC_HASH],
             lk->dyn_off[DSEC_HASH], bytebuf_len(&lk->dynbuf[DSEC_HASH]), 0, 0, 8, 0);
    dyn_shdr(out, nm[DX_DYNSYM], SHT_DYNSYM, SHF_ALLOC, lk->dyn_addr[DSEC_DYNSYM],
             lk->dyn_off[DSEC_DYNSYM], bytebuf_len(&lk->dynbuf[DSEC_DYNSYM]), DX_DYNSTR, 1, 8,
             sizeof(Elf64_Sym));
    dyn_shdr(out, nm[DX_DYNSTR], SHT_STRTAB, SHF_ALLOC, lk->dyn_addr[DSEC_DYNSTR],
             lk->dyn_off[DSEC_DYNSTR], bytebuf_len(&lk->dynbuf[DSEC_DYNSTR]), 0, 0, 1, 0);
    dyn_shdr(out, nm[DX_RELA_DYN], SHT_RELA, SHF_ALLOC, lk->dyn_addr[DSEC_RELA_DYN],
             lk->dyn_off[DSEC_RELA_DYN], bytebuf_len(&lk->dynbuf[DSEC_RELA_DYN]), DX_DYNSYM, 0, 8,
             sizeof(Elf64_Rela));
    dyn_shdr(out, nm[DX_RELA_PLT], SHT_RELA, SHF_ALLOC, lk->dyn_addr[DSEC_RELA_PLT],
             lk->dyn_off[DSEC_RELA_PLT], bytebuf_len(&lk->dynbuf[DSEC_RELA_PLT]), DX_DYNSYM, DX_PLT,
             8, sizeof(Elf64_Rela));
    dyn_shdr(out, nm[DX_PLT], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, lk->dyn_addr[DSEC_PLT],
             lk->dyn_off[DSEC_PLT], bytebuf_len(&lk->dynbuf[DSEC_PLT]), 0, 0, 16, 16);
    dyn_shdr(out, nm[DX_TEXT], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, lk->out[OUT_TEXT].addr,
             lk->out[OUT_TEXT].offset, lk->out[OUT_TEXT].size, 0, 0, 16, 0);
    dyn_shdr(out, nm[DX_RODATA], SHT_PROGBITS, SHF_ALLOC, lk->out[OUT_RODATA].addr,
             lk->out[OUT_RODATA].offset, lk->out[OUT_RODATA].size, 0, 0, 8, 0);
    dyn_shdr(out, nm[DX_EH], SHT_PROGBITS, SHF_ALLOC, lk->out[OUT_EH_FRAME].addr,
             lk->out[OUT_EH_FRAME].offset, lk->out[OUT_EH_FRAME].size, 0, 0, 8, 0);
    dyn_shdr(out, nm[DX_INIT], SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE, lk->out[OUT_INIT_ARRAY].addr,
             lk->out[OUT_INIT_ARRAY].offset, lk->out[OUT_INIT_ARRAY].size, 0, 0, 8, 8);
    dyn_shdr(out, nm[DX_FINI], SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE, lk->out[OUT_FINI_ARRAY].addr,
             lk->out[OUT_FINI_ARRAY].offset, lk->out[OUT_FINI_ARRAY].size, 0, 0, 8, 8);
    dyn_shdr(out, nm[DX_DYNAMIC], SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, lk->dyn_addr[DSEC_DYNAMIC],
             lk->dyn_off[DSEC_DYNAMIC], bytebuf_len(&lk->dynbuf[DSEC_DYNAMIC]), DX_DYNSTR, 0, 8,
             sizeof(Elf64_Dyn));
    dyn_shdr(out, nm[DX_GOT], SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, lk->dyn_addr[DSEC_GOT],
             lk->dyn_off[DSEC_GOT], bytebuf_len(&lk->dynbuf[DSEC_GOT]), 0, 0, 8, 8);
    dyn_shdr(out, nm[DX_GOT_PLT], SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, lk->dyn_addr[DSEC_GOT_PLT],
             lk->dyn_off[DSEC_GOT_PLT], bytebuf_len(&lk->dynbuf[DSEC_GOT_PLT]), 0, 0, 8, 8);
    dyn_shdr(out, nm[DX_DATA], SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, lk->out[OUT_DATA].addr,
             lk->out[OUT_DATA].offset, lk->out[OUT_DATA].size, 0, 0, 8, 0);
    dyn_shdr(out, nm[DX_BSS], SHT_NOBITS, SHF_ALLOC | SHF_WRITE, lk->out[OUT_BSS].addr,
             lk->out[OUT_BSS].offset, lk->out[OUT_BSS].size, 0, 0, 8, 0);
    dyn_shdr(out, nm[DX_SYMTAB], SHT_SYMTAB, 0, 0, off_symtab, size_symtab, DX_STRTAB, 1, 8,
             sizeof(Elf64_Sym));
    dyn_shdr(out, nm[DX_STRTAB], SHT_STRTAB, 0, 0, off_strtab, size_strtab, 0, 0, 1, 0);
    dyn_shdr(out, nm[DX_SHSTRTAB], SHT_STRTAB, 0, 0, off_sh, size_sh, 0, 0, 1, 0);
}

static bool write_dynamic(Linker *lk)
{
    Arena *arena = lk->arena;
    ByteBuf sh;
    strtab_init(&sh, arena);
    u32 nm[DX_NSEC];
    nm[DX_NULL] = 0;
    nm[DX_INTERP] = strtab_add(&sh, ".interp");
    nm[DX_HASH] = strtab_add(&sh, ".gnu.hash");
    nm[DX_DYNSYM] = strtab_add(&sh, ".dynsym");
    nm[DX_DYNSTR] = strtab_add(&sh, ".dynstr");
    nm[DX_RELA_DYN] = strtab_add(&sh, ".rela.dyn");
    nm[DX_RELA_PLT] = strtab_add(&sh, ".rela.plt");
    nm[DX_PLT] = strtab_add(&sh, ".plt");
    nm[DX_TEXT] = strtab_add(&sh, ".text");
    nm[DX_RODATA] = strtab_add(&sh, ".rodata");
    nm[DX_EH] = strtab_add(&sh, ".eh_frame");
    nm[DX_INIT] = strtab_add(&sh, ".init_array");
    nm[DX_FINI] = strtab_add(&sh, ".fini_array");
    nm[DX_DYNAMIC] = strtab_add(&sh, ".dynamic");
    nm[DX_GOT] = strtab_add(&sh, ".got");
    nm[DX_GOT_PLT] = strtab_add(&sh, ".got.plt");
    nm[DX_DATA] = strtab_add(&sh, ".data");
    nm[DX_BSS] = strtab_add(&sh, ".bss");
    nm[DX_SYMTAB] = strtab_add(&sh, ".symtab");
    nm[DX_STRTAB] = strtab_add(&sh, ".strtab");
    nm[DX_SHSTRTAB] = strtab_add(&sh, ".shstrtab");

    ByteBuf strtab, symtab;
    strtab_init(&strtab, arena);
    bytebuf_init(&symtab, arena);
    emit_symtab(lk, &symtab, &strtab);

    u64 cursor = align_up(lk->seg_off[3] + lk->seg_filesz[3], 8);
    u64 off_symtab = cursor;
    cursor += bytebuf_len(&symtab);
    u64 off_strtab = cursor;
    cursor += bytebuf_len(&strtab);
    u64 off_sh = cursor;
    cursor += bytebuf_len(&sh);
    u64 shoff = align_up(cursor, 8);

    ByteBuf out;
    bytebuf_init(&out, arena);
    dyn_ehdr(&out, lk->entry, shoff);
    dyn_phdrs(lk, &out);
    append_at(&out, lk->dyn_off[DSEC_INTERP], &lk->dynbuf[DSEC_INTERP]);
    append_at(&out, lk->dyn_off[DSEC_HASH], &lk->dynbuf[DSEC_HASH]);
    append_at(&out, lk->dyn_off[DSEC_DYNSYM], &lk->dynbuf[DSEC_DYNSYM]);
    append_at(&out, lk->dyn_off[DSEC_DYNSTR], &lk->dynbuf[DSEC_DYNSTR]);
    append_at(&out, lk->dyn_off[DSEC_RELA_DYN], &lk->dynbuf[DSEC_RELA_DYN]);
    append_at(&out, lk->dyn_off[DSEC_RELA_PLT], &lk->dynbuf[DSEC_RELA_PLT]);
    append_at(&out, lk->dyn_off[DSEC_PLT], &lk->dynbuf[DSEC_PLT]);
    append_at(&out, lk->out[OUT_TEXT].offset, &lk->out[OUT_TEXT].bytes);
    append_at(&out, lk->out[OUT_RODATA].offset, &lk->out[OUT_RODATA].bytes);
    append_at(&out, lk->out[OUT_EH_FRAME].offset, &lk->out[OUT_EH_FRAME].bytes);
    append_at(&out, lk->out[OUT_GCC_EXCEPT].offset, &lk->out[OUT_GCC_EXCEPT].bytes);
    append_at(&out, lk->out[OUT_INIT_ARRAY].offset, &lk->out[OUT_INIT_ARRAY].bytes);
    append_at(&out, lk->out[OUT_FINI_ARRAY].offset, &lk->out[OUT_FINI_ARRAY].bytes);
    append_at(&out, lk->dyn_off[DSEC_DYNAMIC], &lk->dynbuf[DSEC_DYNAMIC]);
    append_at(&out, lk->dyn_off[DSEC_GOT], &lk->dynbuf[DSEC_GOT]);
    append_at(&out, lk->dyn_off[DSEC_GOT_PLT], &lk->dynbuf[DSEC_GOT_PLT]);
    append_at(&out, lk->out[OUT_DATA].offset, &lk->out[OUT_DATA].bytes);
    append_at(&out, off_symtab, &symtab);
    append_at(&out, off_strtab, &strtab);
    append_at(&out, off_sh, &sh);
    pad_to(&out, shoff);
    dyn_section_headers(lk, &out, nm, off_symtab, bytebuf_len(&symtab), off_strtab,
                        bytebuf_len(&strtab), off_sh, bytebuf_len(&sh));

    const char *path = lk->cfg->output_path ? lk->cfg->output_path : "a.out";
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        link_error("%s: cannot open output", path);
        return false;
    }
    fwrite(bytebuf_data(&out), 1, bytebuf_len(&out), f);
    fclose(f);
    chmod(path, 0755);
    return true;
}

int link_run(const LinkConfig *cfg, Vec *inputs, Arena *arena)
{
    Linker lk;
    linker_init(&lk, cfg, arena);
    for (size_t i = 0; i < vec_size(cfg->lib_paths); i++)
    {
        vec_push(lk.search_dirs, vec_get(cfg->lib_paths, i));
    }
    for (const char *const *d = DEFAULT_LIB_DIRS; *d; d++)
    {
        vec_push(lk.search_dirs, (void *) *d);
    }
    if (!cfg->nostdlib)
    {
        const char *crt1 = find_in_dirs(&lk, "crt1.o");
        if (!crt1)
        {
            link_error("cannot find crt1.o");
            return 1;
        }
        if (!link_add_path(&lk, crt1))
        {
            return 1;
        }
    }
    for (size_t i = 0; i < vec_size(inputs); i++)
    {
        LinkInput *in = (LinkInput *) vec_get(inputs, i);
        if (in->kind == LINK_INPUT_OBJECT)
        {
            if (!merge_object(&lk, in->object))
            {
                return 1;
            }
        }
        else if (!link_add_path(&lk, in->path))
        {
            return 1;
        }
    }
    for (size_t i = 0; i < vec_size(cfg->libs); i++)
    {
        const char *name = (const char *) vec_get(cfg->libs, i);
        const char *path = resolve_library(&lk, name);
        if (!path || !link_add_path(&lk, path))
        {
            return 1;
        }
    }
    if (!cfg->nostdlib)
    {
        const char *libc = resolve_library(&lk, "c");
        if (!libc || !link_add_path(&lk, libc))
        {
            return 1;
        }
    }
    resolve_globals(&lk);
    if (lk.nerrors)
    {
        return 1;
    }
    while (pull_from_archives(&lk))
    {
        resolve_globals(&lk);
        if (lk.nerrors)
        {
            return 1;
        }
    }
    if (lk.dynamic)
    {
        dynamic_scan(&lk);
        if (lk.nerrors)
        {
            return 1;
        }
        dynamic_build(&lk);
        layout_dynamic(&lk);
        finalize_globals(&lk);
        finalize_dynamic(&lk);
        if (!apply_relocs(&lk))
        {
            return 1;
        }
        if (!patch_entry(&lk))
        {
            return 1;
        }
        if (lk.nerrors)
        {
            return 1;
        }
        return write_dynamic(&lk) ? 0 : 1;
    }
    prepare_entry(&lk);
    if (lk.nerrors)
    {
        return 1;
    }
    layout(&lk);
    finalize_globals(&lk);
    if (!apply_relocs(&lk))
    {
        return 1;
    }
    if (!patch_entry(&lk))
    {
        return 1;
    }
    if (lk.nerrors)
    {
        return 1;
    }
    return write_executable(&lk) ? 0 : 1;
}
