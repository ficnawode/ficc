/* Expect exit 42. */

int main(void)
{
    double x = 1.5;
    double y = 2.5;

    if (x + y != 4.0)
    {
        return 1;
    }
    if (x * y != 3.75)
    {
        return 2;
    }
    if (y - x != 1.0)
    {
        return 3;
    }
    if (y / x != 1.6666666666666667)
    {
        return 4;
    }

    float f = 1.5f;
    int i;
    for (i = 0; i < 10; i++)
    {
        f = f * 1.1f;
    }
    if ((int) (f * 1000.0f) != 3890)
    {
        return 5;
    }

    f = 0.0f;
    for (i = 0; i < 200; i++)
    {
        f += 0.1f;
    }
    if ((int) (f * 10.0f) != 200)
    {
        return 6;
    }

    if (!(x < y) || !(y > x) || !(x <= x) || !(x >= x))
    {
        return 7;
    }
    if (x > y || y < x || x == y || x != x)
    {
        return 8;
    }

    /* IEEE-754: only != is true when comparing with NaN. */
    double n = 0.0 / 0.0;
    if (n == n || !(n != n) || n < 1.0 || n > 1.0 || n <= 1.0 || n >= 1.0)
    {
        return 9;
    }

    /* IEEE-754: -0.0 is falsy but equals 0.0; sign survives negation. */
    double z = -0.0;
    if (z != 0.0 || z || -z != 0.0)
    {
        return 10;
    }

    _Bool b = 1e-300;
    if (!b)
    {
        return 11;
    }
    b = z;
    if (b || !(!z))
    {
        return 12;
    }

    x++;
    if (x != 2.5)
    {
        return 13;
    }
    x--;
    if (x != 1.5)
    {
        return 14;
    }

    int c = 0;
    if ((c ? 1 : 1.5) != 1.5)
    {
        return 15;
    }
    if ((c ? 1.5 : 2) != 2.0)
    {
        return 16;
    }

    return 42;
}