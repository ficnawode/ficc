#include "link.h"
#include "x86_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
