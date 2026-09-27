/* Expect exit 42. */

long double g = 1.5L;
long double gsum = 1.5L + 2.5L;
const long double gc = -2.5L;
long double ga = 1.5L;
long double gb = 2.5L;
long double gout;
struct S
{
    char c;
    long double d;
    int i;
};
long double arr[2] = {3.5L, 4.5L};

int main(void)
{
    unsigned long long lo = *(unsigned long long *) &g;
    unsigned long long hi = *(unsigned long long *) ((char *) &g + 8);
    if (lo != 0xC000000000000000ULL || hi != 0x3FFFULL)
    {
        return 1;
    }
    unsigned char *p = (unsigned char *) &g;
    if (p[9] != 0x3F || p[10] != 0 || p[15] != 0)
    {
        return 2;
    }
    lo = *(unsigned long long *) &gsum;
    hi = *(unsigned long long *) ((char *) &gsum + 8);
    if (lo != 0x8000000000000000ULL || hi != 0x4001ULL)
    {
        return 3;
    }
    if (*(unsigned long long *) ((char *) &gc + 8) != 0xC000ULL)
    {
        return 4;
    }

    long double x = 0.0L;
    x = g;
    if (*(unsigned long long *) &x != 0xC000000000000000ULL)
    {
        return 5;
    }
    long double y = gsum;
    if (*(unsigned long long *) &y != 0x8000000000000000ULL)
    {
        return 6;
    }

    int c = *(unsigned char *) ((char *) &ga + 9) == 0x3F;
    gout = c ? ga : gb;
    if (*(unsigned long long *) &gout != 0xC000000000000000ULL)
    {
        return 7;
    }
    gout = c ? gb : ga;
    if (*(unsigned long long *) &gout != 0xA000000000000000ULL)
    {
        return 8;
    }

    struct S s = {1, 2.5L, 3};
    if (sizeof(long double) != 16 || _Alignof(long double) != 16)
    {
        return 9;
    }
    if (sizeof(struct S) != 48 || _Alignof(struct S) != 16)
    {
        return 10;
    }
    if ((unsigned long) &((struct S *) 0)->d != 16 || (unsigned long) &((struct S *) 0)->i != 32)
    {
        return 11;
    }
    if (sizeof(arr) != 32)
    {
        return 12;
    }
    if (*(unsigned long long *) ((char *) &s.d) != 0xA000000000000000ULL)
    {
        return 13;
    }
    if (*(unsigned long long *) ((char *) &s + 32) != 3)
    {
        return 14;
    }

    return 42;
}