/* Expect exit 42. */

long double res;

int main(void)
{
    long double s = 0.0L;
    s = 0.1L + 0.2L;
    if (*(unsigned long long *) &s != 0x999999999999999aULL)
    {
        return 1;
    }
    if (*(unsigned short *) ((char *) &s + 8) != 0x3FFD)
    {
        return 2;
    }
    s = 3.0L / 7.0L;
    if (*(unsigned long long *) &s != 0xdb6db6db6db6db6eULL)
    {
        return 3;
    }
    s = 1.0L / 3.0L;
    if (*(unsigned long long *) &s != 0xaaaaaaaaaaaaaaabULL)
    {
        return 4;
    }
    long double a = 1.5L;
    long double b = 2.5L;
    if (a + b != 4.0L || b - a != 1.0L || a * b != 3.75L || b / a != 1.6666666666666666666L)
    {
        return 5;
    }
    /* a per-step chain (0x1.999999999999999ap-1L is 0.1L; 10 adds) */
    s = 0.0L;
    int i;
    for (i = 0; i < 10; i++)
    {
        s = s + 0.1L;
    }
    if (*(unsigned long long *) &s != 0x8000000000000001ULL)
    {
        return 6;
    }
    if (*(unsigned short *) ((char *) &s + 8) != 0x3FFF)
    {
        return 7;
    }
    if (s == (long double) 1.0L)
    {
        return 8; /* the chain lands a hair above 1.0 */
    }

    if (!(a < b) || !(a <= 2.0L) || !(b > a) || !(b >= 2.5L))
    {
        return 9;
    }
    long double z = 0.0L;
    long double nan = z / z;
    if (nan == nan || nan < 1.0L || nan > 1.0L || nan <= 1.0L || nan >= 1.0L)
    {
        return 10;
    }
    if (!(nan != 1.0L))
    {
        return 11;
    }

    long double nz = -0.0L;
    if (nz)
    {
        return 14;
    }
    if (!(!nz))
    {
        return 15;
    }
    _Bool bb = nz;
    if (bb)
    {
        return 16;
    }
    if ((nz ? 7 : 9) != 9 || (1.0L ? 7 : 9) != 7)
    {
        return 17;
    }
    if (-a != -1.5L)
    {
        return 18;
    }
    a += 2.5L;
    a *= 2.0L;
    a -= 1.0L;
    a /= 2.0L;
    if (a != 3.5L)
    {
        return 19;
    }
    a++;
    if (a != 4.5L)
    {
        return 20;
    }
    --a;
    if (a != 3.5L)
    {
        return 21;
    }

    if ((long double) 3 != 3.0L)
    {
        return 22;
    }
    if ((long double) -7LL != -7.0L)
    {
        return 23;
    }
    long double fconv = (long double) 1.5f;
    if (*(unsigned long long *) &fconv != 0xC000000000000000ULL)
    {
        return 24;
    }
    if ((double) 4.25L != 4.25 || (float) 4.25L != 4.25f)
    {
        return 25;
    }
    if ((int) 4.25L != 4 || (long long) -4.5L != -4)
    {
        return 26;
    }
    if ((unsigned) 4.25L != 4U || (unsigned long long) 4.25L != 4ULL)
    {
        return 27;
    }

    long double big = (long double) 0xFFFFFFFFFFFFFFFFULL;
    if (*(unsigned long long *) &big != 0xFFFFFFFFFFFFFFFFULL)
    {
        return 28;
    }
    if (*(unsigned short *) ((char *) &big + 8) != 0x403E)
    {
        return 29;
    }
    long double five = (long double) 0x8000000000000005ULL;
    if (*(unsigned long long *) &five != 0x8000000000000005ULL)
    {
        return 30;
    }
    if (*(unsigned short *) ((char *) &five + 8) != 0x403E)
    {
        return 31;
    }
    if ((unsigned long long) big != 0xFFFFFFFFFFFFFFFFULL)
    {
        return 32;
    }
    if ((unsigned long long) 9223372036854775808.0L != 0x8000000000000000ULL)
    {
        return 33;
    }
    if ((long long) nan != (-9223372036854775807LL - 1))
    {
        return 34;
    }
    if ((int) nan != -2147483647 - 1)
    {
        return 35;
    }

    /* subnormal literal (smallest positive extended) */
    long double sub = 0x1p-16445L;
    if (*(unsigned long long *) &sub != 0x0000000000000001ULL || sub == 0.0L)
    {
        return 36;
    }

    res = a < b ? 1.5L : 4.5L;
    if (*(unsigned long long *) &res != 0x9000000000000000ULL)
    {
        return 37;
    }

    return 42;
}