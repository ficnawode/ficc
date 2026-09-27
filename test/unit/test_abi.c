#include "abi.h"
#include "harness.h"
#include "type.h"
#include "util/arena.h"
#include "util/vec.h"
#include "x86_sysv.h"

static RecordField *field(Arena *a, const char *name, Type *t)
{
    RecordField *rf = arena_alloc(a, sizeof(RecordField), _Alignof(RecordField));
    rf->name = name;
    rf->type = t;
    rf->offset = 0;
    rf->bit_offset = -1;
    rf->bit_width = -1;
    rf->align_override = 0;
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
    /* psABI §3.2.3 rule 5c: the 24-byte misaligned aggregate is MEMORY. */
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
    /* psABI §3.2.3: the field straddling the boundary folds lane 0 to INTEGER. */
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
    /* psABI §3.2.3: the union lanes fold to the max member class (INTEGER). */
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
    /* psABI §3.2.3 rule 5c: 17 bytes exceeds two eightbytes, so MEMORY. */
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

TEST(abi, sysv_plan_record_lanes_and_scalar)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "c", type_char()));
    vec_push(fs, field(a, "d", type_double()));
    Type *cd = ctx_struct("A22PlanCD", fs);

    Type *types[2] = {cd, type_long()};
    SysvArgPlan plans[2];
    u32 fp_used = 0;
    u32 stack = sysv_plan_args(types, 2, plans, &fp_used);
    EXPECT_EQ(stack, 0);
    EXPECT_EQ(fp_used, 1);
    EXPECT_TRUE(plans[0].is_record);
    EXPECT_TRUE(plans[0].register_passed);
    EXPECT_EQ(plans[0].nchunks, 2);
    EXPECT_EQ(plans[0].chunks[0].kind, SYSV_GP);
    EXPECT_EQ(plans[0].chunks[0].chunk_off, 0);
    EXPECT_EQ(plans[0].chunks[1].kind, SYSV_SSE);
    EXPECT_EQ(plans[0].chunks[1].chunk_off, 8);
    EXPECT_EQ(plans[1].nchunks, 1);
    EXPECT_EQ(plans[1].chunks[0].kind, SYSV_GP);
    EXPECT_EQ(plans[1].chunks[0].reg, 1);
    arena_free(a);
}

TEST(abi, sysv_plan_integer_overflow_rides_stack)
{
    Type *types[7];
    for (int i = 0; i < 7; i++)
    {
        types[i] = type_long();
    }
    SysvArgPlan plans[7];
    u32 fp_used = 0;
    u32 stack = sysv_plan_args(types, 7, plans, &fp_used);
    EXPECT_EQ(stack, 8);
    EXPECT_FALSE(plans[5].on_stack);
    EXPECT_TRUE(plans[6].on_stack);
    EXPECT_EQ(plans[6].stack_off, 0);
    EXPECT_EQ(plans[6].stack_size, 8);
}

TEST(abi, sysv_plan_record_overflows_when_gp_regs_run_out)
{
    Arena *a = arena_new();
    Vec *fs = vec_new(a);
    vec_push(fs, field(a, "a", type_long()));
    vec_push(fs, field(a, "b", type_long()));
    Type *pair = ctx_struct("A22PlanPair", fs);

    Type *types[6] = {type_long(), type_long(), type_long(), type_long(), type_long(), pair};
    SysvArgPlan plans[6];
    u32 fp_used = 0;
    u32 stack = sysv_plan_args(types, 6, plans, &fp_used);
    EXPECT_EQ(stack, 16);
    EXPECT_TRUE(plans[5].is_record);
    EXPECT_FALSE(plans[5].register_passed);
    EXPECT_TRUE(plans[5].on_stack);
    EXPECT_EQ(plans[5].stack_off, 0);
    arena_free(a);
}

TEST(abi, sysv_plan_long_double_rides_aligned_stack)
{
    Type *types[1] = {type_long_double()};
    SysvArgPlan plans[1];
    u32 fp_used = 0;
    u32 stack = sysv_plan_args(types, 1, plans, &fp_used);
    EXPECT_EQ(stack, 16);
    EXPECT_TRUE(plans[0].on_stack);
    EXPECT_TRUE(plans[0].is_x87_stack);
    EXPECT_EQ(plans[0].stack_off, 0);
}

TEST(abi, sysv_plan_sse_overflow_rides_stack)
{
    Type *types[9];
    for (int i = 0; i < 9; i++)
    {
        types[i] = type_double();
    }
    SysvArgPlan plans[9];
    u32 fp_used = 0;
    u32 stack = sysv_plan_args(types, 9, plans, &fp_used);
    EXPECT_EQ(stack, 8);
    EXPECT_EQ(fp_used, 8);
    EXPECT_FALSE(plans[7].on_stack);
    EXPECT_TRUE(plans[8].on_stack);
    EXPECT_EQ(plans[8].stack_off, 0);
    EXPECT_EQ(plans[8].stack_size, 8);
}

TEST(abi, sysv_plan_is_deterministic)
{
    Type *types[3] = {type_double(), type_double(), type_long()};
    SysvArgPlan p1[3], p2[3];
    u32 f1 = 0, f2 = 0;
    u32 s1 = sysv_plan_args(types, 3, p1, &f1);
    u32 s2 = sysv_plan_args(types, 3, p2, &f2);
    EXPECT_EQ(s1, s2);
    EXPECT_EQ(f1, f2);
    for (int i = 0; i < 3; i++)
    {
        EXPECT_EQ(p1[i].nchunks, p2[i].nchunks);
        EXPECT_EQ(p1[i].on_stack, p2[i].on_stack);
        EXPECT_EQ(p1[i].stack_off, p2[i].stack_off);
    }
}