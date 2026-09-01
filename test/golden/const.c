/* Phase 9: const qualifier — end-to-end golden test.
   Compile with ficc, link the object with gcc -no-pie, and expect exit status 42. */

const int LIMIT = 40;           /* const global -> .rodata (no SHF_WRITE) */
const char *const GREET = "hi"; /* const pointer to string -> .rodata + .rela.rodata */
int g = 12;                     /* writable scalar global for &-of-global */
int lo_val = 10;
int hi_val = 19;

struct point
{
    int x;
    int y;
};

int sum_of(struct point const *p) /* pointer-to-const-struct parameter */
{
    return p->x + p->y; /* reads only — writes are rejected by semantic */
}

int delta(int const *a, int const *b) /* top-level const pointer parameters */
{
    return *b - *a;
}

int step(void)
{
    static const int s = 2; /* const block-scope static -> .rodata */
    return s;
}

int main(void)
{
    const int local = 5; /* const local (block scope) */
    const int *p = &g;   /* pointer to const from a scalar global address */

    int spill = 11; /* block-scope auto spilled because &spill is taken */
    int *sp = &spill;
    *sp = 21; /* write through the pointer to the spill slot */

    struct point pt;
    pt.x = 1;
    pt.y = 2;
    int s = sum_of(&pt); /* int* -> struct point const* (qualifiers added) */

    int d = delta(&lo_val, &hi_val); /* int* -> const int* arguments */

    /* 21 + 5 + 40 + 3 + 9 + 2 + 104 - 142 = 42 */
    return spill + local + LIMIT + s + d + step() + GREET[0] + *p - 12 - 142;
}
