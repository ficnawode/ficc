union Mix
{
    int i;
    char c;
    long l;
};

struct Pt
{
    int x;
    int y;
};

union Wrap
{
    struct Pt p;
    long l;
};

int main(void)
{
    if (sizeof(union Mix) != 8)
    {
        return 1;
    }
    if (sizeof(union Wrap) != 8)
    {
        return 2;
    }
    union Mix m;
    m.i = 65;
    if (m.c != 65)
    {
        return 3;
    }
    m.c = 66;
    if (m.i != 66)
    {
        return 4;
    }
    union Wrap w;
    w.p.x = 5;
    w.p.y = 6;
    return w.p.x * 10 + w.p.y; /* 56 */
}
