struct Pair
{
    int a;
    int b;
};

struct Pair bump(struct Pair p)
{
    p.a = p.a + 1;
    p.b = p.b + 1;
    return p;
}

int main(void)
{
    struct Pair x;
    x.a = 10;
    x.b = 20;
    struct Pair y = bump(x);
    return x.a + x.b + y.a + y.b; /* 10 + 20 + 11 + 21 = 62 */
}
