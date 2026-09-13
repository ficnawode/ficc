/* long double — type, literals, constants, value model. End-to-end golden.
   Compile with ficc (`ficc -run float_long_double.c` for the interpreter,
   `ficc -c` + `gcc -no-pie` for the ELF), and expect exit status 42.

   Exercises the surface on *storage* only (no FP arithmetic/compares, which
   land later): file-scope and .rodata initializer bytes, folded constant
   expressions, address-taken and SSA copies, width-16 phi copies into a global
   store, and sizeof/_Alignof/record layout — verified bit-for-bit via byte
   casts so interp == ELF wherever codegen touches 16-byte moves.

   Reference: 80-bit double-extended (fraction explicitly carries the integer
   bit). 1.5L = fraction 0xC000000000000000 @ exp 0x3FFF; 4.0L = 0x80.. › @
   0x4001; 2.5L = 0xA000000000000000 @ 0x4000; 4.5L = 0x9000000000000000 @
   0x4001; -2.5L flips byte 9's sign bit (0xC0). */

long double g = 1.5L;
long double gsum = 1.5L + 2.5L; /* 4.0L */
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

    /* Block-scope copies: an address-taken local and a plain SSA value. */
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

    /* A width-16 phi feeding a global store (`gout = c ? ga : gb`). */
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

    /* Record / array layout honors the 16-byte member alignment. */
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