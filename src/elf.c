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
    ByteBuf shstrtab;
    strtab_init(&shstrtab, arena);
    u32 shname_text = strtab_add(&shstrtab, ".text");
    u32 shname_symtab = strtab_add(&shstrtab, ".symtab");
    u32 shname_strtab = strtab_add(&shstrtab, ".strtab");
    u32 shname_shstrtab = strtab_add(&shstrtab, ".shstrtab");

    ByteBuf strtab;
    strtab_init(&strtab, arena);

    /* Build .text content (concatenate all functions) */
    ByteBuf text;
    bytebuf_init(&text, arena);
    size_t nfuncs = vec_size(cm->funcs);
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        bytebuf_append_bytes(&text, bytebuf_data(cf->bytes), bytebuf_len(cf->bytes));
    }

    /* Build .symtab content */
    ByteBuf symtab;
    bytebuf_init(&symtab, arena);
    /* Index 0: null symbol */
    for (size_t i = 0; i < sizeof(Elf64_Sym); i++)
    {
        bytebuf_append(&symtab, 0);
    }
    /* Index 1: .text section symbol (local) */
    bytebuf_append_u32(&symtab, 0);                                 /* st_name */
    bytebuf_append(&symtab, ELF64_ST_INFO(STB_LOCAL, STT_SECTION)); /* st_info */
    bytebuf_append(&symtab, 0);                                     /* st_other */
    bytebuf_append_u16(&symtab, SEC_TEXT);                          /* st_shndx */
    bytebuf_append_u64(&symtab, 0);                                 /* st_value */
    bytebuf_append_u64(&symtab, 0);                                 /* st_size */
    /* IrGlobal function symbols */
    for (size_t i = 0; i < nfuncs; i++)
    {
        CodegenFunc *cf = (CodegenFunc *) vec_get(cm->funcs, i);
        u32 name_off = strtab_add(&strtab, cf->name);
        bytebuf_append_u32(&symtab, name_off);                        /* st_name */
        bytebuf_append(&symtab, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC)); /* st_info */
        bytebuf_append(&symtab, 0);                                   /* st_other */
        bytebuf_append_u16(&symtab, SEC_TEXT);                        /* st_shndx */
        bytebuf_append_u64(&symtab, cf->offset);                      /* st_value */
        bytebuf_append_u64(&symtab, bytebuf_len(cf->bytes));          /* st_size */
    }

    /* Compute layout */
    size_t off = 0;
    /* ELF header */
    size_t off_ehdr = off;
    off += sizeof(Elf64_Ehdr);
    /* .text */
    size_t off_text = off;
    off += bytebuf_len(&text);
    /* .symtab */
    bytebuf_align(&symtab, 8); /* ensure symtab content is aligned */
    size_t off_symtab = (off + 7) & ~7;
    off = off_symtab + bytebuf_len(&symtab);
    /* .strtab */
    size_t off_strtab = off;
    off += bytebuf_len(&strtab);
    /* .shstrtab */
    size_t off_shstrtab = off;
    off += bytebuf_len(&shstrtab);
    /* Section header table */
    size_t off_shdr = (off + 7) & ~7;

    /* Write ELF header */
    ByteBuf out;
    bytebuf_init(&out, arena);
    /* e_ident */
    bytebuf_append(&out, ELFMAG0);
    bytebuf_append(&out, ELFMAG1);
    bytebuf_append(&out, ELFMAG2);
    bytebuf_append(&out, ELFMAG3);
    bytebuf_append(&out, ELFCLASS64);
    bytebuf_append(&out, ELFDATA2LSB);
    bytebuf_append(&out, EV_CURRENT);
    bytebuf_append(&out, 0); /* ELFOSABI */
    for (int i = 0; i < 8; i++)
    {
        bytebuf_append(&out, 0);
    }
    bytebuf_append_u16(&out, ET_REL);
    bytebuf_append_u16(&out, EM_X86_64);
    bytebuf_append_u32(&out, EV_CURRENT);
    bytebuf_append_u64(&out, 0);                  /* e_entry */
    bytebuf_append_u64(&out, 0);                  /* e_phoff */
    bytebuf_append_u64(&out, off_shdr);           /* e_shoff */
    bytebuf_append_u32(&out, 0);                  /* e_flags */
    bytebuf_append_u16(&out, sizeof(Elf64_Ehdr)); /* e_ehsize */
    bytebuf_append_u16(&out, 0);                  /* e_phentsize */
    bytebuf_append_u16(&out, 0);                  /* e_phnum */
    bytebuf_append_u16(&out, sizeof(Elf64_Shdr)); /* e_shentsize */
    bytebuf_append_u16(&out, SEC_COUNT);          /* e_shnum */
    bytebuf_append_u16(&out, SEC_SHSTRTAB);       /* e_shstrndx */

    /* Write sections */
    bytebuf_append_bytes(&out, bytebuf_data(&text), bytebuf_len(&text));
    bytebuf_align(&out, 8);
    bytebuf_append_bytes(&out, bytebuf_data(&symtab), bytebuf_len(&symtab));
    bytebuf_append_bytes(&out, bytebuf_data(&strtab), bytebuf_len(&strtab));
    bytebuf_append_bytes(&out, bytebuf_data(&shstrtab), bytebuf_len(&shstrtab));
    bytebuf_align(&out, 8);

    /* Write section headers */
    size_t shdr_base = bytebuf_len(&out);
    (void) shdr_base;
    (void) off_ehdr;
    (void) off_text;
    (void) off_symtab;
    (void) off_strtab;
    (void) off_shstrtab;

    /* SHT_NULL */
    bytebuf_append_u32(&out, 0);        /* sh_name */
    bytebuf_append_u32(&out, SHT_NULL); /* sh_type */
    bytebuf_append_u64(&out, 0);        /* sh_flags */
    bytebuf_append_u64(&out, 0);        /* sh_addr */
    bytebuf_append_u64(&out, 0);        /* sh_offset */
    bytebuf_append_u64(&out, 0);        /* sh_size */
    bytebuf_append_u32(&out, 0);        /* sh_link */
    bytebuf_append_u32(&out, 0);        /* sh_info */
    bytebuf_append_u64(&out, 0);        /* sh_addralign */
    bytebuf_append_u64(&out, 0);        /* sh_entsize */

    /* .text */
    bytebuf_append_u32(&out, shname_text);
    bytebuf_append_u32(&out, SHT_PROGBITS);
    bytebuf_append_u64(&out, SHF_ALLOC | SHF_EXECINSTR);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, off_text);
    bytebuf_append_u64(&out, bytebuf_len(&text));
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u64(&out, 1);
    bytebuf_append_u64(&out, 0);

    /* .symtab */
    bytebuf_append_u32(&out, shname_symtab);
    bytebuf_append_u32(&out, SHT_SYMTAB);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, off_symtab);
    bytebuf_append_u64(&out, bytebuf_len(&symtab));
    bytebuf_append_u32(&out, SEC_STRTAB); /* sh_link = .strtab */
    bytebuf_append_u32(&out, 2);          /* sh_info = last local + 1 */
    bytebuf_append_u64(&out, 8);
    bytebuf_append_u64(&out, sizeof(Elf64_Sym));

    /* .strtab */
    bytebuf_append_u32(&out, shname_strtab);
    bytebuf_append_u32(&out, SHT_STRTAB);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, off_strtab);
    bytebuf_append_u64(&out, bytebuf_len(&strtab));
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u64(&out, 1);
    bytebuf_append_u64(&out, 0);

    /* .shstrtab */
    bytebuf_append_u32(&out, shname_shstrtab);
    bytebuf_append_u32(&out, SHT_STRTAB);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, 0);
    bytebuf_append_u64(&out, off_shstrtab);
    bytebuf_append_u64(&out, bytebuf_len(&shstrtab));
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u32(&out, 0);
    bytebuf_append_u64(&out, 1);
    bytebuf_append_u64(&out, 0);

    fwrite(bytebuf_data(&out), 1, bytebuf_len(&out), f);
    fclose(f);
    arena_free(arena);
}
