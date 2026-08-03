#include "elf.h"
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

#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_FUNC 2
#define STT_SECTION 3
#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

#define SHN_UNDEF 0

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

/* ---- output byte buffer ---- */
typedef struct ByteBuf ByteBuf;
struct ByteBuf
{
    u8 *data;
    size_t len;
    size_t cap;
    Arena *arena;
};

static void bb_init(ByteBuf *bb, Arena *arena)
{
    bb->arena = arena;
    bb->len = 0;
    bb->cap = 256;
    bb->data = arena_alloc(arena, bb->cap, 1);
}

static void bb_grow(ByteBuf *bb, size_t need)
{
    if (bb->len + need > bb->cap)
    {
        while (bb->len + need > bb->cap)
            bb->cap *= 2;
        u8 *old = bb->data;
        bb->data = arena_alloc(bb->arena, bb->cap, 1);
        memcpy(bb->data, old, bb->len);
    }
}

static void bb_append(ByteBuf *bb, u8 byte)
{
    bb_grow(bb, 1);
    bb->data[bb->len++] = byte;
}

static void bb_append_bytes(ByteBuf *bb, const u8 *src, size_t n)
{
    bb_grow(bb, n);
    memcpy(bb->data + bb->len, src, n);
    bb->len += n;
}

static void bb_u16(ByteBuf *bb, u16 v)
{
    bb_append(bb, (u8) (v & 0xFF));
    bb_append(bb, (u8) ((v >> 8) & 0xFF));
}

static void bb_u32(ByteBuf *bb, u32 v)
{
    bb_u16(bb, (u16) (v & 0xFFFF));
    bb_u16(bb, (u16) ((v >> 16) & 0xFFFF));
}

static void bb_u64(ByteBuf *bb, u64 v)
{
    bb_u32(bb, (u32) (v & 0xFFFFFFFF));
    bb_u32(bb, (u32) (v >> 32));
}

static void bb_align(ByteBuf *bb, size_t align)
{
    if (align == 0)
        return;
    size_t mask = align - 1;
    size_t padded = (bb->len + mask) & ~mask;
    while (bb->len < padded)
        bb_append(bb, 0);
}

/* ---- string table builder ---- */
typedef struct Strtab Strtab;
struct Strtab
{
    ByteBuf buf;
};

static void strtab_init(Strtab *st, Arena *arena)
{
    bb_init(&st->buf, arena);
    bb_append(&st->buf, 0);
}

static u32 strtab_add(Strtab *st, const char *s)
{
    u32 off = (u32) st->buf.len;
    size_t n = strlen(s);
    bb_append_bytes(&st->buf, (const u8 *) s, n);
    bb_append(&st->buf, 0);
    return off;
}

typedef enum
{
    SEC_NULL = 0,
    SEC_TEXT,
    SEC_SYMTAB,
    SEC_STRTAB,
    SEC_SHSTRTAB,
    SEC_COUNT
} SectionIndex;

#define SYMTAB_ENTSIZE sizeof(Elf64_Sym)
#define SHDR_ENTSIZE sizeof(Elf64_Shdr)

void elf_write(CodegenModule *cm, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }

    Arena *arena = arena_new();

    /* Build string tables */
    Strtab shstrtab;
    strtab_init(&shstrtab, arena);
    u32 shname_text = strtab_add(&shstrtab, ".text");
    u32 shname_symtab = strtab_add(&shstrtab, ".symtab");
    u32 shname_strtab = strtab_add(&shstrtab, ".strtab");
    u32 shname_shstrtab = strtab_add(&shstrtab, ".shstrtab");

    Strtab strtab;
    strtab_init(&strtab, arena);

    /* Build .text content (concatenate all functions) */
    ByteBuf text;
    bb_init(&text, arena);
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        bb_append_bytes(&text, cf->bytes, cf->len);
    }

    /* Build .symtab content */
    ByteBuf symtab;
    bb_init(&symtab, arena);
    /* Index 0: null symbol */
    for (size_t i = 0; i < sizeof(Elf64_Sym); i++)
        bb_append(&symtab, 0);
    /* Index 1: .text section symbol (local) */
    bb_u32(&symtab, 0);                                        /* st_name */
    bb_append(&symtab, ELF64_ST_INFO(STB_LOCAL, STT_SECTION)); /* st_info */
    bb_append(&symtab, 0);                                     /* st_other */
    bb_u16(&symtab, SEC_TEXT);                                 /* st_shndx */
    bb_u64(&symtab, 0);                                        /* st_value */
    bb_u64(&symtab, 0);                                        /* st_size */
    /* Global function symbols */
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        u32 name_off = strtab_add(&strtab, cf->name);
        bb_u32(&symtab, name_off);                               /* st_name */
        bb_append(&symtab, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC)); /* st_info */
        bb_append(&symtab, 0);                                   /* st_other */
        bb_u16(&symtab, SEC_TEXT);                               /* st_shndx */
        bb_u64(&symtab, cf->offset);                             /* st_value */
        bb_u64(&symtab, cf->len);                                /* st_size */
    }

    /* Compute layout */
    size_t off = 0;
    /* ELF header */
    size_t off_ehdr = off;
    off += sizeof(Elf64_Ehdr);
    /* .text */
    size_t off_text = off;
    off += text.len;
    /* .symtab */
    bb_align(&symtab, 8); /* ensure symtab content is aligned */
    size_t off_symtab = (off + 7) & ~7;
    off = off_symtab + symtab.len;
    /* .strtab */
    size_t off_strtab = off;
    off += strtab.buf.len;
    /* .shstrtab */
    size_t off_shstrtab = off;
    off += shstrtab.buf.len;
    /* Section header table */
    size_t off_shdr = (off + 7) & ~7;

    /* Write ELF header */
    ByteBuf out;
    bb_init(&out, arena);
    /* e_ident */
    bb_append(&out, ELFMAG0);
    bb_append(&out, ELFMAG1);
    bb_append(&out, ELFMAG2);
    bb_append(&out, ELFMAG3);
    bb_append(&out, ELFCLASS64);
    bb_append(&out, ELFDATA2LSB);
    bb_append(&out, EV_CURRENT);
    bb_append(&out, 0); /* ELFOSABI */
    for (int i = 0; i < 8; i++)
        bb_append(&out, 0);
    bb_u16(&out, ET_REL);
    bb_u16(&out, EM_X86_64);
    bb_u32(&out, EV_CURRENT);
    bb_u64(&out, 0);                  /* e_entry */
    bb_u64(&out, 0);                  /* e_phoff */
    bb_u64(&out, off_shdr);           /* e_shoff */
    bb_u32(&out, 0);                  /* e_flags */
    bb_u16(&out, sizeof(Elf64_Ehdr)); /* e_ehsize */
    bb_u16(&out, 0);                  /* e_phentsize */
    bb_u16(&out, 0);                  /* e_phnum */
    bb_u16(&out, sizeof(Elf64_Shdr)); /* e_shentsize */
    bb_u16(&out, SEC_COUNT);          /* e_shnum */
    bb_u16(&out, SEC_SHSTRTAB);       /* e_shstrndx */

    /* Write sections */
    bb_append_bytes(&out, text.data, text.len);
    bb_align(&out, 8);
    bb_append_bytes(&out, symtab.data, symtab.len);
    bb_append_bytes(&out, strtab.buf.data, strtab.buf.len);
    bb_append_bytes(&out, shstrtab.buf.data, shstrtab.buf.len);
    bb_align(&out, 8);

    /* Write section headers */
    size_t shdr_base = out.len;
    (void) shdr_base;
    (void) off_ehdr;
    (void) off_text;
    (void) off_symtab;
    (void) off_strtab;
    (void) off_shstrtab;

    /* SHT_NULL */
    bb_u32(&out, 0);        /* sh_name */
    bb_u32(&out, SHT_NULL); /* sh_type */
    bb_u64(&out, 0);        /* sh_flags */
    bb_u64(&out, 0);        /* sh_addr */
    bb_u64(&out, 0);        /* sh_offset */
    bb_u64(&out, 0);        /* sh_size */
    bb_u32(&out, 0);        /* sh_link */
    bb_u32(&out, 0);        /* sh_info */
    bb_u64(&out, 0);        /* sh_addralign */
    bb_u64(&out, 0);        /* sh_entsize */

    /* .text */
    bb_u32(&out, shname_text);
    bb_u32(&out, SHT_PROGBITS);
    bb_u64(&out, SHF_ALLOC | SHF_EXECINSTR);
    bb_u64(&out, 0);
    bb_u64(&out, off_text);
    bb_u64(&out, text.len);
    bb_u32(&out, 0);
    bb_u32(&out, 0);
    bb_u64(&out, 1);
    bb_u64(&out, 0);

    /* .symtab */
    bb_u32(&out, shname_symtab);
    bb_u32(&out, SHT_SYMTAB);
    bb_u64(&out, 0);
    bb_u64(&out, 0);
    bb_u64(&out, off_symtab);
    bb_u64(&out, symtab.len);
    bb_u32(&out, SEC_STRTAB); /* sh_link = .strtab */
    bb_u32(&out, 2);          /* sh_info = last local + 1 */
    bb_u64(&out, 8);
    bb_u64(&out, sizeof(Elf64_Sym));

    /* .strtab */
    bb_u32(&out, shname_strtab);
    bb_u32(&out, SHT_STRTAB);
    bb_u64(&out, 0);
    bb_u64(&out, 0);
    bb_u64(&out, off_strtab);
    bb_u64(&out, strtab.buf.len);
    bb_u32(&out, 0);
    bb_u32(&out, 0);
    bb_u64(&out, 1);
    bb_u64(&out, 0);

    /* .shstrtab */
    bb_u32(&out, shname_shstrtab);
    bb_u32(&out, SHT_STRTAB);
    bb_u64(&out, 0);
    bb_u64(&out, 0);
    bb_u64(&out, off_shstrtab);
    bb_u64(&out, shstrtab.buf.len);
    bb_u32(&out, 0);
    bb_u32(&out, 0);
    bb_u64(&out, 1);
    bb_u64(&out, 0);

    fwrite(out.data, 1, out.len, f);
    fclose(f);
    arena_free(arena);
}
