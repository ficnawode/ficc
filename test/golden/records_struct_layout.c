struct Layout
{
    char c;
    int i;
    char d;
};

int main(void)
{
    /* sizeof(struct Layout): c@0, pad, i@4, d@8, pad -> 12 */
    if (sizeof(struct Layout) != 12)
    {
        return 1;
    }
    struct Layout l;
    l.c = 1;
    l.i = 200;
    l.d = 3;
    return l.c + l.i + l.d;
}
