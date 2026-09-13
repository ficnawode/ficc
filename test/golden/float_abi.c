/* SysV FP ABI + file-scope FP globals, end-to-end golden. Expect exit 42. */

double g = 1.5;
const double gc = 6.25;
double gsum = 1.5 + 2.5;
double garr[3] = {1.0, 2.0, 3.0};

double sum10(double a, double b, double c, double d, double e, double f, double g, double h,
             double i, double j)
{
    return a + b + c + d + e + f + g + h + i + j;
}

double mix(int a, double b, int c, double d)
{
    return a + b + c + d;
}

double vsum(double first, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, first);
    double s = first;
    s += __builtin_va_arg(ap, double);
    s += __builtin_va_arg(ap, double);
    __builtin_va_end(ap);
    return s;
}

float fsum(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    float s = 0.0f;
    for (int i = 0; i < n; i++)
    {
        s += __builtin_va_arg(ap, float);
    }
    __builtin_va_end(ap);
    return s;
}

int main(void)
{
    if (g != 1.5 || gc != 6.25 || gsum != 4.0)
    {
        return 1;
    }
    if (garr[2] != 3.0)
    {
        return 2;
    }
    if (mix(1, 2.0, 3, 4.0) != 10.0)
    {
        return 3;
    }
    if (sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 55.0)
    {
        return 4;
    }
    if (vsum(1.0, 2.0, 3.0) != 6.0)
    {
        return 5;
    }
    if (fsum(4, 0.5f, 1.5f, 2.5f, 3.5f) != 8.0f)
    {
        return 6;
    }
    return 42;
}