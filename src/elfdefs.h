#ifndef FICC_ELFDEFS_H
#define FICC_ELFDEFS_H

#include "util/types.h"

typedef uint8_t Elf64_Byte;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3

#define EM_X86_64 62

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP 17
#define SHT_GNU_HASH 0x6ffffff6
#define SHT_GNU_VERDEF 0x6ffffffd
#define SHT_GNU_VERNEED 0x6ffffffe
#define SHT_GNU_VERSYM 0x6fffffff

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_INFO_LINK 0x40
#define SHF_TLS 0x400

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6
#define STT_GNU_IFUNC 10

#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0xf)
#define ELF64_ST_VISIBILITY(other) ((other) & 0x3)

#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_DEBUG 21
#define DT_FLAGS_1 0x6ffffffb
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM 0x6ffffff0
#define DT_RELACOUNT 0x6ffffff9
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define VER_NDX_LOCAL 0
#define VER_NDX_GLOBAL 1
#define VERSYM_HIDDEN 0x8000
#define VERSYM_VERSION 0x7fff
#define VER_FLG_BASE 0x1
#define VER_FLG_WEAK 0x2

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_8 14
#define R_X86_64_PC8 15
#define R_X86_64_PC64 24
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

#define ELF64_R_SYM(info) ((info) >> 32)
#define ELF64_R_TYPE(info) ((info) & 0xffffffffu)
#define ELF64_R_INFO(sym, type) (((u64) (sym) << 32) + (u32) (type))

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

typedef struct Elf64_Phdr Elf64_Phdr;
struct Elf64_Phdr
{
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

typedef struct Elf64_Dyn Elf64_Dyn;
struct Elf64_Dyn
{
    Elf64_Sxword d_tag;
    union
    {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
};

typedef struct Elf64_Verneed Elf64_Verneed;
struct Elf64_Verneed
{
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
};

typedef struct Elf64_Vernaux Elf64_Vernaux;
struct Elf64_Vernaux
{
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
};

typedef struct Elf64_Verdef Elf64_Verdef;
struct Elf64_Verdef
{
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
};

typedef struct Elf64_Verdaux Elf64_Verdaux;
struct Elf64_Verdaux
{
    Elf64_Word vda_name;
    Elf64_Word vda_next;
};

#endif
