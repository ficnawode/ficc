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
/* Symtab index of .text: target for every .eh_frame FDE relocation. */
#define TEXT_SECTION_SYM 1

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

/* .symtab layout. 0 = null, 1-4 = section symbols; then all STB_LOCAL
   symbols first (string/static globals, then static functions); then the
   globals (non-local globals, then defined functions); then SHN_UNDEF extern
   function symbols. BFD/ld treat every symbol below sh_info as local and
   every symbol at or above it as global (matching st_info), so sh_info must
   be exactly the first non-local index. */
typedef struct
{
    size_t nlocal_globals;
    size_t nstatic_funcs;
    size_t nglobal_vars;
    size_t nglobal_funcs;
    u32 first_global;
} SymLayout;

static SymLayout sym_layout(size_t nglobals, Vec *globals, size_t nfuncs, Vec *funcs)
{
    SymLayout l = {0};
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(globals, i);
        if (g->linkage == IR_LINK_LOCAL)
        {
            l.nlocal_globals++;
        }
        else
        {
            l.nglobal_vars++;
        }
    }
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(funcs, fi);
        if (cf->is_static)
        {
            l.nstatic_funcs++;
        }
        else
        {
            l.nglobal_funcs++;
        }
    }
    l.first_global = FIRST_GLOBAL_SYM + (u32) l.nlocal_globals + (u32) l.nstatic_funcs;
    return l;
}

/* Symbol-table index of the global vector element gi (locals and non-locals
   are interleaved in cm->globals). */
static u32 global_sym_index(Vec *globals, size_t gi, const SymLayout *l)
{
    IrGlobal *g = (IrGlobal *) vec_get(globals, gi);
    size_t pos = 0;
    for (size_t i = 0; i < gi; i++)
    {
        IrGlobal *o = (IrGlobal *) vec_get(globals, i);
        if ((o->linkage == IR_LINK_LOCAL) == (g->linkage == IR_LINK_LOCAL))
        {
            pos++;
        }
    }
    if (g->linkage == IR_LINK_LOCAL)
    {
        return FIRST_GLOBAL_SYM + (u32) pos;
    }
    return l->first_global + (u32) pos;
}

static CodegenFunc *find_codegen_func_elf(CodegenModule *cm, const char *name)
{
    size_t n = vec_size(cm->funcs);
    for (size_t i = 0; i < n; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        if (strcmp(cf->name, name) == 0)
        {
            return cf;
        }
    }
    return NULL;
}

/* Symbol-table index of a function name: a defined in-module function
   (static in the local region, non-static among the globals) or a
   declaration-only extern (after the defined functions). Returns 0 if not
   found (only the null-symbol index 0 is a valid fallback). */
static u32 func_sym_index(CodegenModule *cm, const char *name, Vec *extern_syms, const SymLayout *l)
{
    size_t sfunc = 0, gfunc = 0;
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        if (strcmp(cf->name, name) == 0)
        {
            if (cf->is_static)
            {
                return FIRST_GLOBAL_SYM + (u32) l->nlocal_globals + (u32) sfunc;
            }
            return l->first_global + (u32) l->nglobal_vars + (u32) gfunc;
        }
        if (cf->is_static)
        {
            sfunc++;
        }
        else
        {
            gfunc++;
        }
    }
    size_t nuniq = vec_size(extern_syms);
    for (size_t j = 0; j < nuniq; j++)
    {
        if (strcmp((const char *) vec_get(extern_syms, j), name) == 0)
        {
            return l->first_global + (u32) l->nglobal_vars + (u32) l->nglobal_funcs + (u32) j;
        }
    }
    return 0;
}

void elf_write(CodegenModule *cm, const char *path, const CfiOutput *cfi)
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
    u32 shname_eh_frame = 0;
    u32 shname_rela_eh_frame = 0;
    if (cfi)
    {
        /* Debug sections append after rela_rodata; base indices stay fixed. */
        shname_eh_frame = strtab_add(&shstrtab, ".eh_frame");
        shname_rela_eh_frame = strtab_add(&shstrtab, ".rela.eh_frame");
    }

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

    /* .symtab: 0 = null, 1-4 = section symbols; local globals and static
       functions first (indices below sh_info), then globals, then externs. */
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
    SymLayout layout = sym_layout(nglobals, cm->globals, nfuncs, cm->funcs);
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        if (g->linkage != IR_LINK_LOCAL)
        {
            continue;
        }
        u32 name_off = g->name ? strtab_add(&strtab, g->name) : 0;
        sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE), section_shndx(g->section),
                 global_off[i], type_sizeof(g->type));
    }
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        if (!cf->is_static)
        {
            continue;
        }
        u32 name_off = strtab_add(&strtab, cf->name);
        sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_LOCAL, STT_FUNC), SEC_TEXT, cf->offset,
                 bytebuf_len(cf->bytes));
    }
    for (size_t i = 0; i < nglobals; i++)
    {
        IrGlobal *g = (IrGlobal *) vec_get(cm->globals, i);
        if (g->linkage == IR_LINK_LOCAL)
        {
            continue;
        }
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
        if (cf->is_static)
        {
            continue;
        }
        u32 name_off = strtab_add(&strtab, cf->name);
        sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), SEC_TEXT, cf->offset,
                 bytebuf_len(cf->bytes));
    }

    /* Declaration-only extern functions referenced by calls *or* whose address
       is taken (Phase 16): one SHN_UNDEF STB_GLOBAL symbol per unique name,
       placed after the defined-function symbols; elf's symbol index for extern
       e is FIRST_GLOBAL_SYM + nglobals + nfuncs + e. */
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
    /* Function-address loads (`&f`/designator, D16.1) of *extern* functions
       also need an undefined symbol, even if never called. */
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, fi);
        size_t nfp = vec_size(cf->func_patches);
        for (size_t pi = 0; pi < nfp; pi++)
        {
            FuncAddrPatch *fp = (FuncAddrPatch *) vec_get(cf->func_patches, pi);
            if (find_codegen_func_elf(cm, fp->name))
            {
                continue; /* defined in-module — no undefined symbol needed */
            }
            bool seen = false;
            size_t nuniq = vec_size(extern_syms);
            for (size_t j = 0; j < nuniq; j++)
            {
                if (strcmp((const char *) vec_get(extern_syms, j), fp->name) == 0)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                vec_push(extern_syms, (void *) fp->name);
                u32 name_off = strtab_add(&strtab, fp->name);
                sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), SHN_UNDEF, 0, 0);
            }
        }
    }
    /* File-scope function-pointer initializers (D16.1): a data-side
       R_X86_64_64 relocation against an *extern* function also needs an
       undefined symbol. */
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
            if (!gr->is_func || find_codegen_func_elf(cm, gr->func_name))
            {
                continue;
            }
            bool seen = false;
            size_t nuniq = vec_size(extern_syms);
            for (size_t j = 0; j < nuniq; j++)
            {
                if (strcmp((const char *) vec_get(extern_syms, j), gr->func_name) == 0)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                vec_push(extern_syms, (void *) gr->func_name);
                u32 name_off = strtab_add(&strtab, gr->func_name);
                sym_emit(&symtab, name_off, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), SHN_UNDEF, 0, 0);
            }
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
            rela_emit(&rela_text, cf->offset + gp->offset,
                      global_sym_index(cm->globals, gp->global_index, &layout), R_X86_64_32S, 0);
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
                sym_idx = layout.first_global + (u32) layout.nglobal_vars +
                          (u32) layout.nglobal_funcs + (u32) j;
                break;
            }
        }
        rela_emit(&rela_text, ec->text_offset, sym_idx, R_X86_64_PLT32, -4);
    }

    /* Function-address loads (`&f`/designator, D16.1): R_X86_64_32S against
       the function's own symbol (defined or SHN_UNDEF extern). */
    for (size_t fi = 0; fi < nfuncs; fi++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, fi);
        size_t nfp = vec_size(cf->func_patches);
        for (size_t pi = 0; pi < nfp; pi++)
        {
            FuncAddrPatch *fp = (FuncAddrPatch *) vec_get(cf->func_patches, pi);
            u32 sym_idx = func_sym_index(cm, fp->name, extern_syms, &layout);
            rela_emit(&rela_text, cf->offset + fp->offset, sym_idx, R_X86_64_32S, 0);
        }
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
            u32 sym_idx;
            if (gr->is_func)
            {
                /* A function-address initializer (D16.1): R_X86_64_64 against
                   the function's symbol. */
                sym_idx = func_sym_index(cm, gr->func_name, extern_syms, &layout);
            }
            else
            {
                sym_idx = global_sym_index(cm->globals, gr->target, &layout);
            }
            rela_emit(target, global_off[i] + gr->offset, sym_idx, R_X86_64_64, 0);
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
    size_t off_eh_frame = 0;
    size_t off_rela_eh_frame = 0;
    u64 eh_frame_size = 0;
    u64 rela_eh_frame_size = 0;
    u16 nsections = SEC_COUNT;
    if (cfi)
    {
        eh_frame_size = bytebuf_len(&cfi->eh_frame);
        rela_eh_frame_size = (u64) vec_size(cfi->relocs) * sizeof(Elf64_Rela);
        off_eh_frame = align_up(off, 8);
        off = off_eh_frame + eh_frame_size;
        off_rela_eh_frame = off;
        off += rela_eh_frame_size;
        nsections += 2;
    }
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
    bytebuf_append_u16(&out, nsections);
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
    if (cfi)
    {
        /* .eh_frame plus its RELA against the .text section symbol. */
        while ((size_t) bytebuf_len(&out) < off_eh_frame)
        {
            bytebuf_append(&out, 0);
        }
        bytebuf_append_bytes(&out, bytebuf_data(&cfi->eh_frame), bytebuf_len(&cfi->eh_frame));
        for (size_t i = 0; i < vec_size(cfi->relocs); i++)
        {
            CfiReloc *rel = (CfiReloc *) vec_get(cfi->relocs, i);
            rela_emit(&out, rel->offset, TEXT_SECTION_SYM, R_X86_64_64, rel->addend);
        }
    }
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
              layout.first_global, 8, sizeof(Elf64_Sym));
    shdr_emit(&out, shname_strtab, SHT_STRTAB, 0, off_strtab, bytebuf_len(&strtab), 0, 0, 1, 0);
    shdr_emit(&out, shname_shstrtab, SHT_STRTAB, 0, off_shstrtab, bytebuf_len(&shstrtab), 0, 0, 1,
              0);
    shdr_emit(&out, shname_rela_text, SHT_RELA, SHF_INFO_LINK, off_rela_text,
              bytebuf_len(&rela_text), SEC_SYMTAB, SEC_TEXT, 8, sizeof(Elf64_Rela));
    shdr_emit(&out, shname_rela_data, SHT_RELA, SHF_INFO_LINK, off_rela_data,
              bytebuf_len(&rela_data), SEC_SYMTAB, SEC_DATA, 8, sizeof(Elf64_Rela));
    shdr_emit(&out, shname_rela_rodata, SHT_RELA, SHF_INFO_LINK, off_rela_rodata,
              bytebuf_len(&rela_rodata), SEC_SYMTAB, SEC_RODATA, 8, sizeof(Elf64_Rela));
    if (cfi)
    {
        shdr_emit(&out, shname_eh_frame, SHT_PROGBITS, SHF_ALLOC, off_eh_frame, eh_frame_size, 0, 0,
                  8, 0);
        shdr_emit(&out, shname_rela_eh_frame, SHT_RELA, SHF_INFO_LINK, off_rela_eh_frame,
                  rela_eh_frame_size, SEC_SYMTAB, SEC_COUNT, 8, sizeof(Elf64_Rela));
    }

    fwrite(bytebuf_data(&out), 1, bytebuf_len(&out), f);
    fclose(f);
    arena_free(arena);
}
