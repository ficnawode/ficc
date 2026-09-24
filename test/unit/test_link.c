#include "harness.h"
#include "testdriver.h"

#include "codegen.h"
#include "elf.h"
#include "link.h"
#include "util/bytebuf.h"

#include <unistd.h>

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

static unsigned int link_seq;

static void link_temp_path(char *buf, size_t n, const char *suffix)
{
    snprintf(buf, n, "/tmp/ficc_link_%06u_%s", link_seq++, suffix);
}

static int link_and_run(const char *const *srcs, size_t nsrcs, const char *expected_unused)
{
    (void) expected_unused;
    Arena *arena = arena_new();
    Vec *inputs = vec_new(arena);
    for (size_t i = 0; i < nsrcs; i++)
    {
        ByteBuf *buf = serialize(srcs[i], arena);
        LinkObject *obj = link_read_memory(bytebuf_data(buf), bytebuf_len(buf), "unit.o", arena);
        LinkInput *in = arena_alloc(arena, sizeof(*in), sizeof(void *));
        in->kind = LINK_INPUT_OBJECT;
        in->object = obj;
        in->path = NULL;
        vec_push(inputs, in);
    }
    char bin[128];
    link_temp_path(bin, sizeof(bin), "bin");
    LinkConfig cfg = {0};
    cfg.output_path = bin;
    cfg.lib_paths = vec_new(arena);
    cfg.libs = vec_new(arena);
    cfg.nostdlib = true;
    int rc = link_run(&cfg, inputs, arena);
    if (rc != 0)
    {
        arena_free(arena);
        return -1;
    }
    int exit_code = tc_run_shell(bin);
    unlink(bin);
    arena_free(arena);
    return exit_code;
}

TEST(link, static_link_runs)
{
    const char *src = "int main(void){return 42;}\n";
    EXPECT_EQ(link_and_run(&src, 1, NULL), 42);
}

TEST(link, static_link_cross_tu)
{
    const char *srcs[2] = {
        "extern int triple(int);\nint main(void){return triple(7);}\n",
        "int triple(int x){return x * 3;}\n",
    };
    EXPECT_EQ(link_and_run(srcs, 2, NULL), 21);
}

static u8 *read_file_bytes(const char *path, size_t *out_len, Arena *arena)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    rewind(f);
    u8 *buf = arena_alloc(arena, (size_t) end + 1, 8);
    size_t n = fread(buf, 1, (size_t) end, f);
    fclose(f);
    *out_len = n;
    return buf;
}

TEST(link, deterministic_output)
{
    Arena *arena = arena_new();
    ByteBuf *buf = serialize("static int g = 3;\nint main(void){return g * 14;}\n", arena);
    LinkObject *obj = link_read_memory(bytebuf_data(buf), bytebuf_len(buf), "unit.o", arena);

    char p1[128], p2[128];
    link_temp_path(p1, sizeof(p1), "det1");
    link_temp_path(p2, sizeof(p2), "det2");

    for (int pass = 0; pass < 2; pass++)
    {
        Vec *inputs = vec_new(arena);
        LinkInput *in = arena_alloc(arena, sizeof(*in), sizeof(void *));
        in->kind = LINK_INPUT_OBJECT;
        in->object = obj;
        in->path = NULL;
        vec_push(inputs, in);
        LinkConfig cfg = {0};
        cfg.output_path = pass == 0 ? p1 : p2;
        cfg.lib_paths = vec_new(arena);
        cfg.libs = vec_new(arena);
        cfg.nostdlib = true;
        EXPECT_EQ(link_run(&cfg, inputs, arena), 0);
    }

    size_t n1 = 0, n2 = 0;
    u8 *b1 = read_file_bytes(p1, &n1, arena);
    u8 *b2 = read_file_bytes(p2, &n2, arena);
    EXPECT_TRUE(b1 != NULL && b2 != NULL);
    EXPECT_EQ(n1, n2);
    EXPECT_TRUE(n1 > 0 && memcmp(b1, b2, n1) == 0);

    unlink(p1);
    unlink(p2);
    arena_free(arena);
}
