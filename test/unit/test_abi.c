#include "abi.h"
#include "harness.h"
#include "type.h"
#include "util/arena.h"
#include "util/vec.h"

static RecordField *field(Arena *a, const char *name, Type *t)
{
    RecordField *rf = arena_alloc(a, sizeof(RecordField), _Alignof(RecordField));
    rf->name = name;
    rf->type = t;
    rf->offset = 0;
    rf->bit_offset = -1;
    rf->bit_width = -1;
    return rf;
}

static Type *ctx_struct(const char *tag, Vec *fields)
{
    Type *s = type_record(TYPE_STRUCT, tag);
    type_record_complete(s, fields);
    return s;
}

TEST(abi, char_double_struct_is_gp_plus_xmm)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "c", type_char()));
    vec_push(fs, field(a, "d", type_double()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22CD", fs));
    EXPECT_EQ(eb.neightbytes, 2);
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_EQ(eb.classes[1], AC_SSE);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    EXPECT_FALSE(sysv_eightbyte_return_sret(&eb));
    EXPECT_EQ(sysv_eightbyte_gp_count(&eb), 1);
    EXPECT_EQ(sysv_eightbyte_xmm_count(&eb), 1);
    arena_free(a);
}

TEST(abi, sixteen_byte_int_pair_returns_two_gprs)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "a", type_long()));
    vec_push(fs, field(a, "b", type_long()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22LL", fs));
    EXPECT_EQ(eb.neightbytes, 2);
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_EQ(eb.classes[1], AC_INTEGER);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    EXPECT_FALSE(sysv_eightbyte_return_sret(&eb)); /* RAX:RDX, not sret */
    EXPECT_EQ(sysv_eightbyte_gp_count(&eb), 2);
    arena_free(a);
}

TEST(abi, nine_byte_char_array_is_two_integer_lanes)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "a", type_array(type_char(), 9)));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22C9", fs));
    EXPECT_EQ(eb.neightbytes, 2);
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_EQ(eb.classes[1], AC_INTEGER);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    arena_free(a);
}

TEST(abi, misaligned_three_eightbyte_struct_is_memory)
{
    /* char at 0, long at 1 (misaligned, unaligned-field rule), int at 16:
       24 bytes still exceeds the two-eightbyte register payload (rule 5c). */
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "c", type_char()));
    vec_push(fs, field(a, "l", type_long()));
    vec_push(fs, field(a, "i", type_int()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22CLI", fs));
    EXPECT_EQ(eb.neightbytes, 1);
    EXPECT_EQ(eb.classes[0], AC_MEMORY);
    EXPECT_FALSE(sysv_eightbyte_register_passed(&eb));
    EXPECT_TRUE(sysv_eightbyte_return_sret(&eb));
    arena_free(a);
}

TEST(abi, double_then_char_struct_is_sse_plus_integer)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "d", type_double()));
    vec_push(fs, field(a, "c", type_char()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22DC", fs));
    EXPECT_EQ(eb.neightbytes, 2);
    EXPECT_EQ(eb.classes[0], AC_SSE);
    EXPECT_EQ(eb.classes[1], AC_INTEGER);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    arena_free(a);
}

TEST(abi, spanning_field_merges_into_both_lanes)
{
    /* int at 0, double at 4: the double straddles the eightbyte boundary, so
       lane 0 folds INTEGER+SSE → INTEGER and lane 1 stays SSE.  gcc and clang
       both pass this as RDI + XMM0, proving the split merges per lane. */
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "i", type_int()));
    vec_push(fs, field(a, "d", type_double()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22ID", fs));
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_EQ(eb.classes[1], AC_SSE);
    EXPECT_EQ(sysv_eightbyte_gp_count(&eb), 1);
    EXPECT_EQ(sysv_eightbyte_xmm_count(&eb), 1);
    arena_free(a);
}

TEST(abi, long_double_is_x87_pair)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "v", type_long_double()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22LD", fs));
    EXPECT_EQ(eb.neightbytes, 2);
    EXPECT_EQ(eb.classes[0], AC_X87);
    EXPECT_EQ(eb.classes[1], AC_X87UP);
    EXPECT_FALSE(sysv_eightbyte_register_passed(&eb)); /* x87 args ride the stack */
    EXPECT_FALSE(sysv_eightbyte_return_sret(&eb));     /* but returns arrive in %st0 */
    arena_free(a);
}

TEST(abi, scalar_int_is_one_gp_lane)
{
    SysVEightByte eb = sysv_eightbyte_split(type_int());
    EXPECT_EQ(eb.neightbytes, 1);
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    EXPECT_EQ(sysv_eightbyte_gp_count(&eb), 1);
}

TEST(abi, scalar_double_is_one_xmm_lane)
{
    SysVEightByte eb = sysv_eightbyte_split(type_double());
    EXPECT_EQ(eb.neightbytes, 1);
    EXPECT_EQ(eb.classes[0], AC_SSE);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    EXPECT_EQ(sysv_eightbyte_xmm_count(&eb), 1);
}

TEST(abi, empty_struct_takes_no_lanes)
{
    Arena *a = arena_new();
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22Empty", vec_new(a)));
    EXPECT_EQ(eb.neightbytes, 0);
    EXPECT_TRUE(sysv_eightbyte_register_passed(&eb));
    arena_free(a);
}

TEST(abi, union_takes_the_max_member_class)
{
    /* `union { long a; double b; }`: both members classify lane 0, folding
       INTEGER (a) and SSE (b) → INTEGER; gcc passes it in %rdi. */
    Arena *a = arena_new();
    Type *u = type_record(TYPE_UNION, "A22UnionLd");
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "l", type_long()));
    vec_push(fs, field(a, "d", type_double()));
    type_record_complete(u, fs);
    SysVEightByte eb = sysv_eightbyte_split(u);
    EXPECT_EQ(eb.neightbytes, 1);
    EXPECT_EQ(eb.classes[0], AC_INTEGER);
    EXPECT_EQ(sysv_eightbyte_gp_count(&eb), 1);
    arena_free(a);
}

TEST(abi, seventeen_byte_struct_is_memory)
{
    /* `{char a; double b; char c}`: 17 bytes exceed the two-eightbyte register
       payload, so rule 5c drops it to memory even though lanes are clean. */
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "a", type_char()));
    vec_push(fs, field(a, "b", type_double()));
    vec_push(fs, field(a, "c", type_char()));
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22CdC", fs));
    EXPECT_EQ(eb.neightbytes, 1);
    EXPECT_EQ(eb.classes[0], AC_MEMORY);
    EXPECT_FALSE(sysv_eightbyte_register_passed(&eb));
    arena_free(a);
}

TEST(abi, over_sixty_four_bytes_is_memory)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "x", type_array(type_long(), 9))); /* 72 bytes */
    SysVEightByte eb = sysv_eightbyte_split(ctx_struct("A22Big", fs));
    EXPECT_EQ(eb.classes[0], AC_MEMORY);
    EXPECT_FALSE(sysv_eightbyte_register_passed(&eb));
    EXPECT_TRUE(sysv_eightbyte_return_sret(&eb));
    arena_free(a);
}

TEST(abi, void_classifies_no_class)
{
    SysVEightByte eb = sysv_eightbyte_split(type_void());
    EXPECT_EQ(eb.neightbytes, 0);
}