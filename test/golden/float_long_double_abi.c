/* SysV X87 ABI: stack-passed ld args, %st0 returns, and overflow-only va_arg. */

long double mul(long double a, long double b)
{
    return a * b + 1.0L;
}

double grad(int i, double d, long double ld, float f, long double l2)
{
    return (double) (i + d + ld + (long double) f + l2);
}

/* Six ints fill the GP regs, then a stack ld between two stack ints. */
long double seven(int a, int b, int c, int d, int e, int f, long double g, int h, int i)
{
    return a + b + c + d + e + f + g + h + i;
}

long double vsum(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    long double s = 0.0L;
    int i;
    for (i = 0; i < n; i++)
    {
        s += __builtin_va_arg(ap, long double);
    }
    __builtin_va_end(ap);
    return s;
}

long double vsum_named_ld(int n, long double named, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, named);
    long double s = named;
    int i;
    for (i = 0; i < n; i++)
    {
        s += __builtin_va_arg(ap, long double);
    }
    __builtin_va_end(ap);
    return s;
}

long double vmix(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    long double s = 0.0L;
    int i;
    for (i = 0; i < n; i++)
    {
        if (i == 0)
        {
            s += __builtin_va_arg(ap, int);
        }
        else if (i == 1)
        {
            s += __builtin_va_arg(ap, double);
        }
        else
        {
            s += __builtin_va_arg(ap, long double);
        }
    }
    __builtin_va_end(ap);
    return s;
}

long double res;

int main(void)
{
    if (mul(1.5L, 2.5L) != 4.75L)
    {
        return 1;
    }
    if (mul(-3.0L, 4.0L) != -11.0L)
    {
        return 2;
    }
    if (grad(2, 1.5, 2.5L, 1.25f, 5.0L) != 12.25)
    {
        return 3;
    }
    /* Nested: the inner %st0 return must survive the outer parameter pushes. */
    if (mul(mul(2.0L, 3.0L), 2.0L) != 15.0L)
    {
        return 4;
    }
    long double eight = mul(1.0L, 7.0L);
    if (*(unsigned long long *) &eight != 0x8000000000000000ULL)
    {
        return 5;
    }
    if (*(unsigned short *) ((char *) &eight + 8) != 0x4002)
    {
        return 6;
    }
    if (seven(1, 2, 3, 4, 5, 6, 7.5L, 8, 9) != 45.5L)
    {
        return 7;
    }
    if (seven(0, 0, 0, 0, 0, 0, -1.25L, 0, 0) != -1.25L)
    {
        return 8;
    }

    if (vsum(3, 1.0L, 2.0L, 3.0L) != 6.0L)
    {
        return 9;
    }
    if (vsum(9, 1.0L, 2.0L, 3.0L, 4.0L, 5.0L, 6.0L, 7.0L, 8.0L, 9.0L) != 45.0L)
    {
        return 10;
    }
    if (vsum_named_ld(3, 0.5L, 1.0L, 2.0L, 3.0L) != 6.5L)
    {
        return 11;
    }
    if (vmix(3, 7, 1.5, 2.5L) != 11.0L)
    {
        return 12;
    }
    if (vmix(3, 1, 2.0, 0.5L) != 3.5L)
    {
        return 13;
    }

    res = mul(3.0L, 4.0L);
    if (*(unsigned long long *) &res != 0xD000000000000000ULL)
    {
        return 14;
    }
    if (*(unsigned short *) ((char *) &res + 8) != 0x4002)
    {
        return 15;
    }

    return 42;
}