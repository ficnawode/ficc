#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "elf.h"
#include "link.h"
#include "util/bytebuf.h"

static ByteBuf *serialize(const char *src, Arena *arena)
{
    IrModule *mod = tc_build_module(src, arena);
    if (!mod)
    {
        return NULL;
    }
    CodegenModule *cm = codegen_ir_to_machine(mod, NULL, arena);
    if (!cm)
    {
        return NULL;
    }
    return elf_serialize(cm, NULL, arena);
}

static const char *unit_src = "extern int ext_fn(int);\n"
                              "int gvar = 7;\n"
                              "int zero;\n"
                              "static int svar = 3;\n"
                              "const char *msg = \"hi\";\n"
                              "int add(int a, int b)\n"
                              "{\n"
                              "    return a + b + ext_fn(a);\n"
                              "}\n"
                              "int main(void)\n"
                              "{\n"
                              "    return add(gvar, svar) + zero;\n"
                              "}\n";

static u64 section_index(const LinkObject *obj, const char *name)
{
    size_t n = vec_size(obj->sections);
    for (size_t i = 0; i < n; i++)
    {
        LinkSection *sec = (LinkSection *) vec_get(obj->sections, i);
        if (strcmp(sec->name, name) == 0)
        {
            return i;
        }
    }
    return (u64) -1;
}

static LinkSym *find_sym(const LinkObject *obj, const char *name)
{
    size_t n = vec_size(obj->symbols);
    for (size_t i = 0; i < n; i++)
    {
        LinkSym *s = (LinkSym *) vec_get(obj->symbols, i);
        if (strcmp(s->name, name) == 0)
        {
            return s;
        }
    }
    return NULL;
}

TEST(link, reader_roundtrip)
{
    Arena *arena = arena_new();
    ByteBuf *buf = serialize(unit_src, arena);
    EXPECT_TRUE(buf != NULL);

    LinkObject *obj = link_read_memory(bytebuf_data(buf), bytebuf_len(buf), "unit.o", arena);
    EXPECT_TRUE(obj != NULL);

    LinkSection *text = link_find_section(obj, ".text");
    EXPECT_TRUE(text != NULL);
    EXPECT_EQ(text->type, SHT_PROGBITS);
    EXPECT_TRUE((text->flags & (SHF_ALLOC | SHF_EXECINSTR)) == (SHF_ALLOC | SHF_EXECINSTR));
    EXPECT_TRUE(text->size > 0);

    LinkSection *data = link_find_section(obj, ".data");
    EXPECT_TRUE(data != NULL && data->size >= 8);
    LinkSection *bss = link_find_section(obj, ".bss");
    EXPECT_TRUE(bss != NULL && bss->type == SHT_NOBITS && bss->data == NULL && bss->size >= 4);
    EXPECT_TRUE(link_find_section(obj, ".rodata") != NULL);
    EXPECT_TRUE(link_find_section(obj, ".symtab") != NULL);

    LinkSym *gvar = find_sym(obj, "gvar");
    EXPECT_TRUE(gvar != NULL);
    EXPECT_EQ(gvar->bind, STB_GLOBAL);
    EXPECT_EQ(gvar->shndx, section_index(obj, ".data"));

    LinkSym *svar = find_sym(obj, "svar");
    EXPECT_TRUE(svar != NULL);
    EXPECT_EQ(svar->bind, STB_LOCAL);

    LinkSym *ext = find_sym(obj, "ext_fn");
    EXPECT_TRUE(ext != NULL);
    EXPECT_EQ(ext->bind, STB_GLOBAL);
    EXPECT_EQ(ext->shndx, SHN_UNDEF);

    u64 text_i = section_index(obj, ".text");
    Vec *relocs = (Vec *) vec_get(obj->relocs, text_i);
    EXPECT_TRUE(relocs != NULL && vec_size(relocs) > 0);

    arena_free(arena);
}

TEST(link, reader_reloc_kinds)
{
    Arena *arena = arena_new();
    ByteBuf *buf = serialize(unit_src, arena);
    LinkObject *obj = link_read_memory(bytebuf_data(buf), bytebuf_len(buf), "unit.o", arena);
    EXPECT_TRUE(obj != NULL);

    u64 text_i = section_index(obj, ".text");
    Vec *relocs = (Vec *) vec_get(obj->relocs, text_i);
    bool saw_plt32 = false, saw_32s = false;
    for (size_t i = 0; i < vec_size(relocs); i++)
    {
        LinkReloc *r = (LinkReloc *) vec_get(relocs, i);
        if (r->type == R_X86_64_PLT32)
        {
            saw_plt32 = true;
            EXPECT_EQ(r->addend, -4);
            EXPECT_STR_EQ(((LinkSym *) vec_get(obj->symbols, r->sym))->name, "ext_fn");
        }
        if (r->type == R_X86_64_32S)
        {
            saw_32s = true;
        }
    }
    EXPECT_TRUE(saw_plt32);
    EXPECT_TRUE(saw_32s);

    arena_free(arena);
}

TEST(link, reader_rejects_truncated)
{
    Arena *arena = arena_new();
    ByteBuf *buf = serialize(unit_src, arena);
    LinkObject *obj = link_read_memory(bytebuf_data(buf), 4, "trunc.o", arena);
    EXPECT_TRUE(obj == NULL);
    arena_free(arena);
}

TEST(link, reader_rejects_bad_machine)
{
    Arena *arena = arena_new();
    ByteBuf *buf = serialize(unit_src, arena);
    u8 *copy = arena_alloc(arena, bytebuf_len(buf), 8);
    memcpy(copy, bytebuf_data(buf), bytebuf_len(buf));
    copy[18] = 0; /* e_machine low byte: no longer EM_X86_64 */
    copy[19] = 0;
    LinkObject *obj = link_read_memory(copy, bytebuf_len(buf), "bad.o", arena);
    EXPECT_TRUE(obj == NULL);
    arena_free(arena);
}
