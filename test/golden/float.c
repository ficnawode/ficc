/* Phase 19c: FP arithmetic and comparisons — end-to-end golden. Compile with
   ficc (`ficc -run float.c` for the interpreter, `ficc -c` + `gcc -no-pie`
   for the ELF), and expect exit status 42.

   Exercises the 19c surface together: float/double add/sub/mul/div (including
   the per-op float re-rounding of `0.1f` chains), every ordered predicate,
   NaN semantics (`x == NaN` is 0, `x != NaN` is 1), `-0.0` truthiness, `_Bool`
   storage via boolify, `++`/`--` on floats, unary minus/logical-not, and
   ternaries mixing int/float branches. */

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

    /* The 0.1f chain re-rounds to float at every step (interp == ELF). */
    f = 0.0f;
    for (i = 0; i < 200; i++)
    {
        f += 0.1f;
    }
    if ((int) (f * 10.0f) != 200)
    {
        return 6;
    }

    /* Every predicate, positive and negative. */
    if (!(x < y) || !(y > x) || !(x <= x) || !(x >= x))
    {
        return 7;
    }
    if (x > y || y < x || x == y || x != x)
    {
        return 8;
    }

    /* NaN: only != is true. */
    double n = 0.0 / 0.0;
    if (n == n || !(n != n) || n < 1.0 || n > 1.0 || n <= 1.0 || n >= 1.0)
    {
        return 9;
    }

    /* -0.0 is falsy but equals 0.0; its sign survives negation. */
    double z = -0.0;
    if (z != 0.0 || z || -z != 0.0)
    {
        return 10;
    }

    /* boolify: _Bool and conditions go through FCMP_NE, never a bit test. */
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

    /* ++/-- step in the FP class. */
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

    /* Mixed-typed ternaries convert the int branch to the FP common type. */
    int c = 0;
    if ((c ? 1 : 1.5) != 1.5)
    {
        return 15;
    }
    if ((c ? 1.5 : 2) != 2.0)
    {
        return 16;
    }

    /* Accumulate to a clean 42. */
    return 42;
}