#include "harness.h"
#include "testdriver.h"

/* Regressions for compiler bugs found while making ficc self-host: each
   program below fails (or crashes) on the pre-fix compiler, and must pass
   through both backends (interp + ELF). */

/* `&&`/`||` guards sealed their enclosing do-while body too early, turning
   loop-carried variables into undefined (0) — here `align` became 0 and the
   mask arithmetic collapsed. */
static const char *short_circuit_guard_src =
    "int main(void) {\n"
    "    unsigned long align = 8;\n"
    "    unsigned long used = 130;\n"
    "    unsigned long mask, pos;\n"
    "    do { if (!(align >= 1 && align <= 16)) { return 1; } } while (0);\n"
    "    do { if (align < 1 || align > 16) { return 3; } } while (0);\n"
    "    mask = align - 1;\n"
    "    pos = (used + mask) & ~mask;\n"
    "    if (pos != 136) { return 2; }\n"
    "    return 0;\n"
    "}\n";

/* Unsigned division/remainder must use `div`, not `idiv`: a dividend with the
   top bit set must not be sign-extended. */
static const char *unsigned_div_src =
    "int main(void) {\n"
    "    unsigned long m = 0xFFFFFFFFFFFFFFFFUL;\n"
    "    if (m / 16UL != 1152921504606846975UL) { return 1; }\n"
    "    if (m % 16UL != 15UL) { return 2; }\n"
    "    if (0x1000000000000000UL / 4UL != 0x400000000000000UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

/* Constant folding must honor unsigned DIV/REM/SHR and relations; folding
   `SIZE_MAX / 16` or `>> 4` with signed semantics yields 0/-1. */
static const char *unsigned_fold_src =
    "static const unsigned long Q = 0xFFFFFFFFFFFFFFFFUL / 16;\n"
    "static const int P = 0xFFFFFFFFFFFFFFFFUL > 0x1000000000000000UL;\n"
    "int main(void) {\n"
    "    if (Q != 1152921504606846975UL) { return 1; }\n"
    "    if (P != 1) { return 2; }\n"
    "    if (0xFFFFFFFFFFFFFFFFUL >> 4 != 1152921504606846975UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

/* Conditions on char-typed values and 64-bit pointers/immediates must be
   tested at their full width, not truncated to 32 bits. */
static const char *wide_compare_src =
    "int main(void) {\n"
    "    const char *s = \"abcdefgh\";\n"
    "    int n = 0;\n"
    "    while (*(s + n)) { n++; }\n"
    "    if (n != 8) { return 1; }\n"
    "    if (s[0] != 'a') { return 2; }\n"
    "    if (!(0xFFFFFFFF00000000UL > 0x7FFFFFFFFFFFFFFFUL)) { return 3; }\n"
    "    if (18446744073709551615UL < 0UL != 0) { return 4; }\n"
    "    if (18446744073709551615UL > 0UL != 1) { return 5; }\n"
    "    return 0;\n"
    "}\n";

/* A loop-carried VALUE_MAX sentinel is a PHI initialized with an immediate;
   the copy must write the full 64-bit slot. */
static const char *phi_width_src =
    "#include <stdint.h>\n"
    "int main(void) {\n"
    "    unsigned long first = SIZE_MAX;\n"
    "    unsigned long i;\n"
    "    for (i = 0; i < 5; i++) {\n"
    "        if (i == 2) { first = i; }\n"
    "        if (i < 2) { if (first != SIZE_MAX) { return 1; } }\n"
    "    }\n"
    "    if (first != 2) { return 2; }\n"
    "    return 0;\n"
    "}\n";

/* Bit-field members initialize via read-modify-write of their storage unit
   (positional, designated, and later assignment with width wrap). */
static const char *bitfield_init_src =
    "struct S { unsigned a:1; unsigned b:2; unsigned c:1; unsigned d:3; };\n"
    "int main(void) {\n"
    "    struct S s = {1, 2, 1, 5};\n"
    "    if (s.a != 1 || s.b != 2 || s.c != 1 || s.d != 5) { return 1; }\n"
    "    struct S t = {.a = 1, .b = 3, .d = 4};\n"
    "    if (t.a != 1 || t.b != 3 || t.c != 0 || t.d != 4) { return 2; }\n"
    "    t.b = 6;\n"
    "    if (t.b != 2 || t.a != 1 || t.c != 0 || t.d != 4) { return 3; }\n"
    "    return 0;\n"
    "}\n";

/* Ordinary union members must not be mistaken for bit-fields: the payload
   write is a full-width store, and a bit-field sibling keeps its RMW. */
static const char *union_payload_src =
    "typedef struct {\n"
    "    int kind;\n"
    "    long pad;\n"
    "    union { long int_val; const char *str; } payload;\n"
    "    unsigned sx:3;\n"
    "} Tok;\n"
    "int main(void) {\n"
    "    Tok t = {.payload = {.int_val = 0xFFFFFFFFFFFFFFFFUL}, .sx = 5};\n"
    "    if (t.payload.int_val != 0xFFFFFFFFFFFFFFFFUL) { return 1; }\n"
    "    if (t.sx != 5) { return 2; }\n"
    "    t.payload.int_val = 0xFFFF0000FFFF0000UL;\n"
    "    if (t.payload.int_val != 0xFFFF0000FFFF0000UL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

/* Big decimal literals (> 2^63) with UL/ULL suffixes must be typed unsigned
   end to end — the lexer's suffix bit-field must survive compilation. */
static const char *big_literal_src =
    "int main(void) {\n"
    "    unsigned long big = 18446744073709551615UL;\n"
    "    unsigned long q = big / 16;\n"
    "    if (q != 1152921504606846975UL) { return 1; }\n"
    "    if (18446744073709551615UL != 0xFFFFFFFFFFFFFFFFUL) { return 2; }\n"
    "    if (18446744073709551615ULL / 16ULL != 1152921504606846975ULL) { return 3; }\n"
    "    return 0;\n"
    "}\n";

TEST(bootstrap, short_circuit_guard)
{
    EXPECT_INTERP_AND_ELF(short_circuit_guard_src, 0);
}

TEST(bootstrap, unsigned_div_remainder)
{
    EXPECT_INTERP_AND_ELF(unsigned_div_src, 0);
}

TEST(bootstrap, unsigned_constant_fold)
{
    EXPECT_INTERP_AND_ELF(unsigned_fold_src, 0);
}

TEST(bootstrap, wide_compare)
{
    EXPECT_INTERP_AND_ELF(wide_compare_src, 0);
}

TEST(bootstrap, phi_sized_sentinel)
{
    EXPECT_INTERP_AND_ELF(phi_width_src, 0);
}

TEST(bootstrap, bitfield_init)
{
    EXPECT_INTERP_AND_ELF(bitfield_init_src, 0);
}

TEST(bootstrap, union_payload_not_bitfield)
{
    EXPECT_INTERP_AND_ELF(union_payload_src, 0);
}

TEST(bootstrap, big_decimal_literal)
{
    EXPECT_INTERP_AND_ELF(big_literal_src, 0);
}