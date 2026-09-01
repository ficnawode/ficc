int main(void)
{
    int a = 1;
    int b = 0;
    int c = 0;
    if (a && b)
    {
        c = 1;
    }
    if (a || b)
    {
        c = c + 10;
    }
    if (b && a)
    {
        c = c + 100;
    }
    if (!a)
    {
        c = c + 1;
    }
    c = c & 15;
    c = c | 8;
    c = c ^ 3;
    c = c << 1;
    c = c >> 1;
    return c;
}
