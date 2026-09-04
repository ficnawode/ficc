/* Phase 14a: _Alignof — end-to-end golden test.
   _Alignof folds to a size_t constant in both backends. */

struct P
{
    char c;
    long d;
};

int main(void)
{
    if (_Alignof(char) != 1)
    {
        return 1;
    }
    if (_Alignof(short) != 2)
    {
        return 2;
    }
    if (_Alignof(int) != 4)
    {
        return 3;
    }
    if (_Alignof(long) != 8)
    {
        return 4;
    }
    if (_Alignof(int *) != 8)
    {
        return 5;
    }
    if (_Alignof(int[4]) != 4)
    {
        return 6;
    }
    if (_Alignof(struct P) != 8)
    {
        return 7;
    }
    if (_Alignof(const long) != 8)
    {
        return 8;
    }

    switch (4)
    {
        case _Alignof(int):
            break;
        default:
            return 9;
    }

    int ga = _Alignof(long);
    if (ga != 8)
    {
        return 10;
    }

    return 0;
}