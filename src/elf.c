#include "elf.h"
#include "util/bytebuf.h"
#include <stdio.h>
#include <string.h>

/* ---- ELF64 format definitions ---- */
typedef uint8_t Elf64_Byte;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_REL 1
#define EM_X86_64 62

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_INFO_LINK 0x40
#define SHF_WRITE 0x1

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_FUNC 2
#define STT_SECTION 3
#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

#define SHN_UNDEF 0

#define R_X86_64_32S 11
#define R_X86_64_64 1
#define R_X86_64_PLT32 4

typedef struct Elf64_Ehdr Elf64_Ehdr;
struct Elf64_Ehdr
{
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

typedef struct Elf64_Shdr Elf64_Shdr;
struct Elf64_Shdr
{
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};

typedef struct Elf64_Sym Elf64_Sym;
struct Elf64_Sym
{
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
};

typedef struct Elf64_Rela Elf64_Rela;
struct Elf64_Rela
{
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
};

/* ---- string table builder ---- */
/* A string table is a byte buffer whose first byte is NUL and whose only
   appends are strings followed by NUL. strtab_add returns the offset. */
static void strtab_init(ByteBuf *st, Arena *arena)
{
    bytebuf_init(st, arena);
    bytebuf_append(st, 0);
}

static u32 strtab_add(ByteBuf *st, const char *s)
{
    u32 off = (u32) bytebuf_len(st);
    size_t n = strlen(s);
    bytebuf_append_bytes(st, (const u8 *) s, n);
    bytebuf_append(st, 0);
    return off;
}

typedef enum
{
    SEC_NULL = 0,
    SEC_TEXT,
    SEC_RODATA,
    SEC_DATA,
    SEC_BSS,
    SEC_SYMTAB,
    SEC_STRTAB,
    SEC_SHSTRTAB,
    SEC_RELA_TEXT,
    SEC_RELA_DATA,
    SEC_RELA_RODATA,
    SEC_COUNT
} SectionIndex;

/* Symbol indices are fixed for determinism (D12):
   0 = null, 1-4 = section symbols (text/rodata/data/bss),
   5+i = global i (definition order), then function symbols. */
#define FIRST_GLOBAL_SYM 5

#define SYMTAB_ENTSIZE sizeof(Elf64_Sym)
#define SHDR_ENTSIZE sizeof(Elf64_Shdr)

static u64 align_up(u64 n, u64 a)
{
    return (n + a - 1) / a * a;
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

static u16 section_shndx(IrSection section)
{
    return section == IR_SECTION_RODATA ? SEC_RODATA
           : section == IR_SECTION_DATA ? SEC_DATA
                                        : SEC_BSS;
}

static void shdr_emit(ByteBuf *out, u32 name, u32 type, u64 flags, u64 offset, u64 size, u32 link,
                      u32 info, u64 addralign, u64 entsize)
{
    bytebuf_append_u32(out, name);
    bytebuf_append_u32(out, type);
    bytebuf_append_u64(out, flags);
    bytebuf_append_u64(out, 0);
    bytebuf_append_u64(out, offset);
    bytebuf_append_u64(out, size);
    bytebuf_append_u32(out, link);
    bytebuf_append_u32(out, info);
    bytebuf_append_u64(out, addralign);
    bytebuf_append_u64(out, entsize);
}

static void rela_emit(ByteBuf *out, u64 offset, u32 sym_idx, u32 type, i64 addend)
{
    bytebuf_append_u64(out, offset);
    bytebuf_append_u64(out, ((u64) sym_idx << 32) | type);
    bytebuf_append_u64(out, (u64) addend);
}

/* One past the last STB_LOCAL symbol index: .symtab sh_info. Section symbols
   (0-4) are local; LOCAL-binding globals (strings, static) extend the local
   region whatever their position. */
static u32 sh_info_first_nonlocal(size_t nglobals, Vec *globals, size_t nfuncs, Vec *funcs)
{
    u32 last_local = FIRST_GLOBAL_SYM;
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(globals, i);
        if (g->linkage == IR_LINK_LOCAL)
        {
            last_local = FIRST_GLOBAL_SYM + (u32) i + 1;
        }
    }
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(funcs, fi);
        if (cf->is_static)
        {
            last_local = FIRST_GLOBAL_SYM + (u32) nglobals + (u32) fi + 1;
        }
    }
    return last_local;
}

void elf_write(CodegenModule *cm, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }

    Arena *arena = arena_new();

    ByteBuf shstrtab;
    strtab_init(&shstrtab, arena);
    u32 shname_text = strtab_add(&shstrtab, ".text");
    u32 shname_rodata = strtab_add(&shstrtab, ".rodata");
    u32 shname_data = strtab_add(&shstrtab, ".data");
    u32 shname_bss = strtab_add(&shstrtab, ".bss");
    u32 shname_symtab = strtab_add(&shstrtab, ".symtab");
    u32 shname_strtab = strtab_add(&shstrtab, ".strtab");
    u32 shname_shstrtab = strtab_add(&shstrtab, ".shstrtab");
    u32 shname_rela_text = strtab_add(&shstrtab, ".rela.text");
    u32 shname_rela_data = strtab_add(&shstrtab, ".rela.data");
    u32 shname_rela_rodata = strtab_add(&shstrtab, ".rela.rodata");

    ByteBuf strtab;
    strtab_init(&strtab, arena);

    size_t nglobals = cm->globals ? vec_size(cm->globals) : 0;

    u64 *global_off = arena_alloc(arena, (nglobals ? nglobals : 1) * sizeof(u64), sizeof(u64));
    ByteBuf text, rodata, data;
    bytebuf_init(&text, arena);
    bytebuf_init(&rodata, arena);
    bytebuf_init(&data, arena);
    u64 bss_size = 0;
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        switch (g->section)
        {
            case IR_SECTION_RODATA:
            {
                u64 off = align_up(bytebuf_len(&rodata), g->align);
                while ((u64) bytebuf_len(&rodata) < off)
                {
                    bytebuf_append(&rodata, 0);
                }
                global_off[i] = off;
                if (g->init_data)
                {
                    bytebuf_append_bytes(&rodata, g->init_data, g->init_len);
                }
                break;
            }
            case IR_SECTION_DATA:
            {
                u64 off = align_up(bytebuf_len(&data), g->align);
                while ((u64) bytebuf_len(&data) < off)
                {
                    bytebuf_append(&data, 0);
                }
                global_off[i] = off;
                if (g->init_data)
                {
                    bytebuf_append_bytes(&data, g->init_data, g->init_len);
                }
                break;
            }
            case IR_SECTION_BSS:
                bss_size = align_up(bss_size, g->align);
                global_off[i] = bss_size;
                bss_size += type_sizeof(g->type);
                break;
        }
    }

    /* .text content */
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        bytebuf_append_bytes(&text, bytebuf_data(cf->bytes), bytebuf_len(cf->bytes));
    }

    /* .symtab: 0 = null, 1-4 = section symbols, 5+i = globals, then functions */
    ByteBuf symtab;
    bytebuf_init(&symtab, arena);
    for (size_t i = 0; i < sizeof(Elf64_Sym); i++)
    {
        bytebuf_append(&symtab, 0);
    }
    sym_emit(&symtab, 0, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), SEC_TEXT, 0, 0);
    sym_emit(&symtab, 0, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), SEC_RODATA, 0, 0);
    sym_emit(&symtab, 0, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), SEC_DATA, 0, 0);
    sym_emit(&symtab, 0, ELF64_ST_INFO(STB_LOCAL, STT_SECTION), SEC_BSS, 0, 0);
    u32 first_nonlocal = sh_info_first_nonlocal(nglobals, cm->globals, nfuncs, cm->funcs);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        u32 name_off = g->name ? strtab_add(&strtab, g->name) : 0;
        u8 bind =
            (g->linkage == IR_LINK_GLOBAL || g->linkage == IR_LINK_EXTERN) ? STB_GLOBAL : STB_LOCAL;
        if (g->linkage == IR_LINK_EXTERN)
        {
            sym_emit(&symtab, name_off, ELF64_ST_INFO(bind, STT_NOTYPE), SHN_UNDEF, 0,
                     type_sizeof(g->type));
        }
        else
        {
            sym_emit(&symtab, name_off, ELF64_ST_INFO(bind, STT_NOTYPE), section_shndx(g->section),
                     global_off[i], type_sizeof(g->type));
        }
    }
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        u32 name_off = strtab_add(&strtab, cf->name);
        sym_emit(&symtab, name_off, ELF64_ST_INFO(cf->is_static ? STB_LOCAL : STB_GLOBAL, STT_FUNC),
                 SEC_TEXT, cf->offset, bytebuf_len(cf->bytes));
    }

    /* Declaration-only extern functions referenced by calls (Phase 16):
       one SHN_UNDEF STB_GLOBAL symbol per unique name, placed after the
       defined-function symbols; elf's symbol index for extern e is
       FIRST_GLOBAL_SYM + nglobals + nfuncs + e. */
    size_t nextern = cm->extern_calls ? vec_size(cm->extern_calls) : 0;
    Vec *extern_syms = vec_new(arena); /* Vec<const char*> — unique names */
    for (size_t i = 0; i < nextern; i++)
    {
        ExternCall *ec = (ExternCall *) vec_get(cm->extern_calls, i);
        bool seen = false;
        size_t nuniq = vec_size(extern_syms);
        for (size_t j = 0; j < nuniq; j++)
        {
            if (strcmp((const char *) vec_get(extern_syms, j), ec->name) == 0)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            vec_push(extern_syms, (void *) ec->name);
            u32 name_off = strtab_add(&strtab, ec->name);
            sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), SHN_UNDEF, 0, 0);
        }
    }

    /* Relocations reference the global's own symbol (5+i), not its section
       symbol, so the linker adjusts for prepended section content. */
    ByteBuf rela_text;
    bytebuf_init(&rela_text, arena);
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, fi);
        for (size_t pi = 0; pi < vec_size(cf->global_patches); pi++)
        {
            GlobalPatch *gp = (GlobalPatch *) vec_get(cf->global_patches, pi);
            rela_emit(&rela_text, cf->offset + gp->offset, FIRST_GLOBAL_SYM + gp->global_index,
                      R_X86_64_32S, 0);
        }
    }

    /* External function calls: R_X86_64_PLT32 against the SHN_UNDEF symbol,
       addend -4 (gcc convention — the rel32 field spans to the next
       instruction). r_offset is the absolute .text offset of the field. */
    for (size_t i = 0; i < nextern; i++)
    {
        ExternCall *ec = (ExternCall *) vec_get(cm->extern_calls, i);
        u32 sym_idx = 0;
        size_t nuniq = vec_size(extern_syms);
        for (size_t j = 0; j < nuniq; j++)
        {
            if (strcmp((const char *) vec_get(extern_syms, j), ec->name) == 0)
            {
                sym_idx = FIRST_GLOBAL_SYM + (u32) nglobals + (u32) nfuncs + (u32) j;
                break;
            }
        }
        rela_emit(&rela_text, ec->text_offset, sym_idx, R_X86_64_PLT32, -4);
    }

    ByteBuf rela_data;
    ByteBuf rela_rodata;
    bytebuf_init(&rela_data, arena);
    bytebuf_init(&rela_rodata, arena);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        if (!g->relocs)
        {
            continue;
        }
        size_t nrelocs = vec_size(g->relocs);
        for (size_t r = 0; r < nrelocs; r++)
        {
            GlobalReloc *gr = (GlobalReloc *) vec_get(g->relocs, r);
            ByteBuf *target = g->section == IR_SECTION_RODATA ? &rela_rodata : &rela_data;
            rela_emit(target, global_off[i] + gr->offset, FIRST_GLOBAL_SYM + gr->target,
                      R_X86_64_64, 0);
        }
    }

    size_t off = sizeof(Elf64_Ehdr);
    size_t off_text = off;
    off += bytebuf_len(&text);
    size_t off_rodata = off;
    off += bytebuf_len(&rodata);
    size_t off_data = align_up(off, 8);
    off = off_data + bytebuf_len(&data);
    size_t off_bss = align_up(off, 8); /* NOBITS: logical only, no file bytes */
    bytebuf_align(&symtab, 8);
    size_t off_symtab = align_up(off, 8);
    off = off_symtab + bytebuf_len(&symtab);
    size_t off_strtab = off;
    off += bytebuf_len(&strtab);
    size_t off_shstrtab = off;
    off += bytebuf_len(&shstrtab);
    size_t off_rela_text = off;
    off += bytebuf_len(&rela_text);
    size_t off_rela_data = off;
    off += bytebuf_len(&rela_data);
    size_t off_rela_rodata = off;
    off += bytebuf_len(&rela_rodata);
    size_t off_shdr = (off + 7) & ~7;

    ByteBuf out;
    bytebuf_init(&out, arena);
    bytebuf_append(&out, ELFMAG0);
    bytebuf_append(&out, ELFMAG1);
    bytebuf_append(&out, ELFMAG2);
    bytebuf_append(&out, ELFMAG3);
    bytebuf_append(&out, ELFCLASS64);
    bytebuf_append(&out, ELFDATA2LSB);
    bytebuf_append(&out, EV_CURRENT);
    bytebuf_append(&out, 0);
    for (int i = 0; i < 8; i++)
    {
        bytebuf_append(&out, 0);
    }
    bytebuf_append_u16(&out, ET_REL);
    bytebuf_append_u16(&out, EM_X86_64);
    bytebuf_append_u32(&out, EV_CURRENT);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, off_shdr);
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u16(&out, sizeof(Elf64_Ehdr));
    bytebuf_append_u16(&out, 0);
    bytebuf_append_u16(&out, 0);
    bytebuf_append_u16(&out, sizeof(Elf64_Shdr));
    bytebuf_append_u16(&out, SEC_COUNT);
    bytebuf_append_u16(&out, SEC_SHSTRTAB);

    /* Section content */
    bytebuf_append_bytes(&out, bytebuf_data(&text), bytebuf_len(&text));
    bytebuf_append_bytes(&out, bytebuf_data(&rodata), bytebuf_len(&rodata));
    while ((size_t) bytebuf_len(&out) < off_data)
    {
        bytebuf_append(&out, 0);
    }
    bytebuf_append_bytes(&out, bytebuf_data(&data), bytebuf_len(&data));
    while ((size_t) bytebuf_len(&out) < off_symtab)
    {
        bytebuf_append(&out, 0);
    }
    bytebuf_append_bytes(&out, bytebuf_data(&symtab), bytebuf_len(&symtab));
    bytebuf_append_bytes(&out, bytebuf_data(&strtab), bytebuf_len(&strtab));
    bytebuf_append_bytes(&out, bytebuf_data(&shstrtab), bytebuf_len(&shstrtab));
    bytebuf_append_bytes(&out, bytebuf_data(&rela_text), bytebuf_len(&rela_text));
    bytebuf_append_bytes(&out, bytebuf_data(&rela_data), bytebuf_len(&rela_data));
    bytebuf_append_bytes(&out, bytebuf_data(&rela_rodata), bytebuf_len(&rela_rodata));
    while ((size_t) bytebuf_len(&out) < off_shdr)
    {
        bytebuf_append(&out, 0);
    }

    shdr_emit(&out, 0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0);
    shdr_emit(&out, shname_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, off_text,
              bytebuf_len(&text), 0, 0, 1, 0);
    shdr_emit(&out, shname_rodata, SHT_PROGBITS, SHF_ALLOC, off_rodata, bytebuf_len(&rodata), 0, 0,
              8, 0);
    shdr_emit(&out, shname_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, off_data, bytebuf_len(&data),
              0, 0, 8, 0);
    shdr_emit(&out, shname_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, off_bss, bss_size, 0, 0, 8, 0);
    shdr_emit(&out, shname_symtab, SHT_SYMTAB, 0, off_symtab, bytebuf_len(&symtab), SEC_STRTAB,
              first_nonlocal, 8, sizeof(Elf64_Sym));
    shdr_emit(&out, shname_strtab, SHT_STRTAB, 0, off_strtab, bytebuf_len(&strtab), 0, 0, 1, 0);
    shdr_emit(&out, shname_shstrtab, SHT_STRTAB, 0, off_shstrtab, bytebuf_len(&shstrtab), 0, 0, 1,
              0);
    shdr_emit(&out, shname_rela_text, SHT_RELA, SHF_INFO_LINK, off_rela_text,
              bytebuf_len(&rela_text), SEC_SYMTAB, SEC_TEXT, 8, sizeof(Elf64_Rela));
    shdr_emit(&out, shname_rela_data, SHT_RELA, SHF_INFO_LINK, off_rela_data,
              bytebuf_len(&rela_data), SEC_SYMTAB, SEC_DATA, 8, sizeof(Elf64_Rela));
    shdr_emit(&out, shname_rela_rodata, SHT_RELA, SHF_INFO_LINK, off_rela_rodata,
              bytebuf_len(&rela_rodata), SEC_SYMTAB, SEC_RODATA, 8, sizeof(Elf64_Rela));

    fwrite(bytebuf_data(&out), 1, bytebuf_len(&out), f);
    fclose(f);
    arena_free(arena);
}
