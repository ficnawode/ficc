_Bool g1 = 42;
_Bool g2 = 0;
struct S
{
    _Bool f;
    int n;
};

_Bool make(int v)
{
    return v;
}

int main(void)
{
    _Bool a = 42;
    _Bool b = 0;
    _Bool c = -1;

    if (a != 1)
    {
        return 1;
    }
    if (b != 0)
    {
        return 2;
    }
    if (c != 1)
    {
        return 3;
    }

    b = 5;
    if (b != 1)
    {
        return 4;
    }

    char ch = 0x77;
    _Bool from_char = ch;
    if (from_char != 1)
    {
        return 5;
    }

    if (g1 != 1)
    {
        return 6;
    }
    if (g2 != 0)
    {
        return 7;
    }

    if (make(9) != 1)
    {
        return 8;
    }
    if (make(0) != 0)
    {
        return 9;
    }

    struct S s = {7, 3};
    if (s.f != 1)
    {
        return 10;
    }
    if (s.n != 3)
    {
        return 11;
    }

    if ((_Bool) 257 != 1)
    {
        return 12;
    }
    if ((_Bool) 0 != 0)
    {
        return 13;
    }

    if (sizeof(_Bool) != 1)
    {
        return 14;
    }
    if (_Alignof(_Bool) != 1)
    {
        return 15;
    }

    return 0;
}