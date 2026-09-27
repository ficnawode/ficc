_Static_assert(1, "always true");
_Static_assert(sizeof(int) == 4, "int is 4 bytes");
_Static_assert(sizeof(long) == 8, "long is 8 bytes");
_Static_assert(_Alignof(long) == 8, "long aligns to 8");

int main(void)
{
    _Static_assert(sizeof(char) == 1, "char in function");
    _Static_assert((1 + 2) * 3 == 9, "arithmetic ICE");
    _Static_assert((char) 300 == 44, "cast wraps to char");

    return 0;
}