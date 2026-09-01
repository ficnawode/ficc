#include "harness.h"
#include "testdriver.h"

static const char *ret42_src = "int main(void) { return 42; }";

TEST(smoke, interp_return42)
{
    EXPECT_EQ(tc_run_interp(ret42_src), 42);
}

TEST(smoke, elf_return42)
{
    EXPECT_EQ(tc_run_elf(ret42_src), 42);
}
