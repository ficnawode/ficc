int counter(void)
{
    static int n = 5; /* initialized once, persists across calls */
    n = n + 1;
    return n;
}

int other(void)
{
    static int n; /* distinct from counter's n, zero-initialized */
    n = n + 2;
    return n;
}

int main(void)
{
    int a = counter();    /* 6 */
    int b = counter();    /* 7 */
    int c = other();      /* 2 */
    int d = other();      /* 4 */
    return a + b + c + d; /* 6 + 7 + 2 + 4 = 19 */
}
