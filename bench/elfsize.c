#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    EI_CLASS = 4,
    EI_DATA = 5,
    ELFCLASS64 = 2,
    ELFDATA2LSB = 1,
    ET_REL = 1,
    ET_EXEC = 2,
    ET_DYN = 3,
    SHT_NOBITS = 8,
    SHN_UNDEF = 0,
};

typedef struct
{
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} SecHdr;

static uint64_t rd(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

static int read_file(const char *path, unsigned char **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "elfsize: cannot open %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0)
    {
        fclose(f);
        return 0;
    }
    unsigned char *buf = malloc((size_t) n + 1);
    if (!buf)
    {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t) n, f) != (size_t) n)
    {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t) n;
    return 1;
}

static int classify_name(const char *name)
{
    if (strcmp(name, ".text") == 0)
    {
        return 1;
    }
    if (strcmp(name, ".data") == 0)
    {
        return 2;
    }
    if (strcmp(name, ".rodata") == 0)
    {
        return 3;
    }
    if (strcmp(name, ".bss") == 0)
    {
        return 4;
    }
    return 0;
}

static int sum_object(const char *path, uint64_t *text, uint64_t *data, uint64_t *rodata,
                      uint64_t *bss)
{
    unsigned char *buf;
    size_t len;
    if (!read_file(path, &buf, &len))
    {
        return 0;
    }
    if (len < 64 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F' ||
        buf[EI_CLASS] != ELFCLASS64 || buf[EI_DATA] != ELFDATA2LSB)
    {
        free(buf);
        return 0;
    }
    uint64_t shoff = rd(buf + 0x28, 8);
    uint16_t shentsize = (uint16_t) rd(buf + 0x3a, 2);
    uint16_t shnum = (uint16_t) rd(buf + 0x3c, 2);
    uint16_t shstrndx = (uint16_t) rd(buf + 0x3e, 2);
    if (shnum == 0 || shstrndx >= shnum || shentsize < 64)
    {
        free(buf);
        return 0;
    }
    const unsigned char *shstr = buf + shoff + (uint64_t) shstrndx * shentsize;
    uint64_t str_off = rd(shstr + 0x18, 8);
    uint64_t str_size = rd(shstr + 0x20, 8);
    if (str_off + str_size > len)
    {
        free(buf);
        return 0;
    }
    for (uint16_t i = 0; i < shnum; i++)
    {
        const unsigned char *sh = buf + shoff + (uint64_t) i * shentsize;
        uint32_t name_off = (uint32_t) rd(sh, 4);
        uint32_t type = (uint32_t) rd(sh + 4, 4);
        uint64_t size = rd(sh + 0x20, 8);
        const char *nm = (const char *) (buf + str_off + name_off);
        if (type == SHT_NOBITS)
        {
            if (classify_name(nm) == 4)
            {
                *bss += size;
            }
            continue;
        }
        switch (classify_name(nm))
        {
            case 1:
                *text += size;
                break;
            case 2:
                *data += size;
                break;
            case 3:
                *rodata += size;
                break;
            default:
                break;
        }
    }
    free(buf);
    return 1;
}

int main(int argc, char **argv)
{
    uint64_t text = 0, data = 0, rodata = 0, bss = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!sum_object(argv[i], &text, &data, &rodata, &bss))
        {
            return 1;
        }
    }
    printf("text %llu data %llu rodata %llu bss %llu\n", (unsigned long long) text,
           (unsigned long long) data, (unsigned long long) rodata, (unsigned long long) bss);
    return 0;
}
