#include "harness.h"

TEST(dummy, always_pass)
{
    EXPECT_TRUE(1 == 1);
    EXPECT_EQ(2 + 2, 4);
}
