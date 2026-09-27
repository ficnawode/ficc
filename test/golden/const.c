/* Expect exit 42. */

const int LIMIT = 40;
const char *const GREET = "hi";
int g = 12;
int lo_val = 10;
int hi_val = 19;

struct point
{
    int x;
    int y;
};

int sum_of(struct point const *p)
{
    return p->x + p->y;
}

int delta(int const *a, int const *b)
{
    return *b - *a;
}

int step(void)
{
    static const int s = 2;
    return s;
}

int main(void)
{
    const int local = 5;
    const int *p = &g;

    int spill = 11;
    int *sp = &spill;
    *sp = 21;

    struct point pt;
    pt.x = 1;
    pt.y = 2;
    int s = sum_of(&pt);

    int d = delta(&lo_val, &hi_val);

    return spill + local + LIMIT + s + d + step() + GREET[0] + *p - 12 - 142;
}
