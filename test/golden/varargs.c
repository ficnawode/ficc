/* Expect exit 42. */

int sum(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    int total = 0;
    for (int i = 0; i < n; i++)
    {
        total += __builtin_va_arg(ap, int);
    }
    __builtin_va_end(ap);
    return total;
}

int read_pair(__builtin_va_list ap)
{
    int a = __builtin_va_arg(ap, int);
    int b = __builtin_va_arg(ap, int);
    return a * 10 + b;
}

int probe(int a, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, a);
    int gp_base = *(int *) ap + *(int *) ((char *) ap + 4); /* gp_offset + fp_offset */
    int pair = read_pair(ap);
    __builtin_va_end(ap);
    return gp_base + pair;
}

int main(void)
{
    if (sum(0) != 0)
    {
        return 1;
    }
    if (sum(5, 1, 2, 3, 4, 5) != 15)
    {
        return 2;
    }
    /* Nine trailing args: five in GP slots, seven on the caller's stack. */
    if (sum(9, 1, 2, 3, 4, 5, 6, 7, 8, 9) != 45)
    {
        return 3;
    }
    /* Zero-offset base probe: gp_offset (8) + fp_offset (48) is invariant, and
       read_pair walks the two trailing args. */
    if (probe(0, 1, 2) != 56 + 12)
    {
        return 4;
    }
    return 42;
}