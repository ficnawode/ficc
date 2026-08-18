int add_one(int n)
{
    {
        int n = n + 100; /* shadows the parameter; discarded on scope exit */
        n = n + 1;
    }
    return n; /* the parameter, unchanged */
}

int main(void)
{
    int x = 1;
    {
        int x = 2; /* shadows outer x */
        x = x + 3; /* inner x = 5 */
    }
    /* outer x == 1 */
    int y = 0;
    if (x)
    {
        int y = 10; /* shadows outer y */
        y = y + 1;
    }
    else
    {
        int y = 20; /* shadows outer y */
        y = y + 1;
    }
    /* outer y == 0 */
    return x + y + add_one(5); /* 1 + 0 + 5 = 6 */
}
