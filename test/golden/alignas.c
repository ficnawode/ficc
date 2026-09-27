_Alignas(16) int g1 = 5;
static _Alignas(8) int g2 = 6;
_Alignas(16) const int g3 = 7;

int main(void)
{
    _Alignas(8) int x = 1;
    static _Alignas(16) int s = 2;
    int _Alignas(4) y = 3;

    if (g1 != 5)
    {
        return 1;
    }
    if (g2 != 6)
    {
        return 2;
    }
    if (g3 != 7)
    {
        return 3;
    }
    if (x != 1)
    {
        return 4;
    }
    if (s != 2)
    {
        return 5;
    }
    if (y != 3)
    {
        return 6;
    }

    return 0;
}