/* Expect exit 42. */

int narrow(void)
{
    if ((char) 300 != 44)
    {
        return 1;
    }
    if ((char) 255 != -1)
    {
        return 2;
    }
    if ((short) 70000 != 4464)
    {
        return 3;
    }
    if ((unsigned char) -1 != 255)
    {
        return 4;
    }
    if ((unsigned short) -1 != 65535)
    {
        return 5;
    }
    return 42;
}

int widen(void)
{
    int neg = -7;
    unsigned int all_ones = (unsigned int) -1;
    if ((long) neg != -7)
    {
        return 1;
    }
    if ((unsigned long) all_ones != 4294967295)
    {
        return 2;
    }
    if ((unsigned long) -1 < 1000000)
    {
        return 3;
    }
    return 42;
}

/* Integer→pointer→integer round trip (implementation-defined but fixed on
   LP64/x86-64: the value is preserved through the 64-bit pointer). */
int int_ptr_int(void)
{
    unsigned long a = 0x1234;
    void *v = (void *) a;
    if ((unsigned long) v != 0x1234)
    {
        return 1;
    }
    int *p = (int *) (void *) (unsigned long) 0x1000;
    if ((unsigned long) p != 0x1000)
    {
        return 2;
    }
    return 42;
}

struct point
{
    int x;
    int y;
};

int ptr_cast(void)
{
    struct point pt;
    pt.x = 11;
    pt.y = 22;
    char *base = (char *) &pt;
    struct point *q = (struct point *) base;
    if (q->y != 22)
    {
        return 1;
    }
    void *v = (void *) &pt;
    if (((struct point *) v)->x != 11)
    {
        return 2;
    }
    char arr[4];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    arr[3] = 4;
    if (*(int *) arr != 0x04030201)
    {
        return 3;
    }
    return 42;
}

void nop(void)
{
}

int discards(void)
{
    int x = 5;
    (void) x;
    (void) 123;
    struct point pt;
    (void) pt;
    (void) nop();
    return 42;
}

enum mode
{
    MO_A,
    MO_B,
    MO_C
};

int take_mode(enum mode m)
{
    return (int) m;
}

int enum_cast(void)
{
    enum mode m = (enum mode) 2;
    if ((int) m != MO_C)
    {
        return 1;
    }
    if (take_mode((enum mode) 1) != MO_B)
    {
        return 2;
    }
    return 42;
}

/* Casts are legal inside case-label integer constant expressions (§6.6), even
   when their type is only known at semantic time (`sizeof(arr)`). */
int case_cast(void)
{
    int r = 0;
    switch ((char) 300)
    {
        case (int) (char) 300:
            r = 42;
            break;
        default:
            r = 1;
            break;
    }
    int arr[10];
    int k = 0;
    switch (40)
    {
        case (int) sizeof(arr):
            k = 42;
            break;
        default:
            k = 1;
            break;
    }
    if (r != 42 || k != 42)
    {
        return 1;
    }
    return 42;
}

/* Const interplay: adding pointee const and dropping it are both legal cast
   conversions. This test only reads through the dropped-const pointer —
   modifying would be undefined (§6.7.3p7), so it never writes. */
int const_cast(void)
{
    int x = 5;
    int *q = &x;
    const int *cp = (const int *) q;
    if (*cp != 5)
    {
        return 1;
    }
    const int ci = 7;
    const int *cc = &ci;
    int *rp = (int *) (void *) cc;
    if (*rp != 7)
    {
        return 2;
    }
    int y = (int) ci;
    if (y != 7)
    {
        return 3;
    }
    return 42;
}

int main(void)
{
    if (narrow() != 42)
    {
        return 10;
    }
    if (widen() != 42)
    {
        return 20;
    }
    if (int_ptr_int() != 42)
    {
        return 30;
    }
    if (ptr_cast() != 42)
    {
        return 40;
    }
    if (discards() != 42)
    {
        return 50;
    }
    if (enum_cast() != 42)
    {
        return 60;
    }
    if (case_cast() != 42)
    {
        return 70;
    }
    if (const_cast() != 42)
    {
        return 80;
    }
    return 42;
}