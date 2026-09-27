/* Expect exit 42. */

typedef int (*Op)(int);

int add10(int x)
{
    return x + 10;
}

int mul2(int x)
{
    return x * 2;
}

int sub7(int x)
{
    return x - 7;
}

static const Op dispatch[3] = {add10, mul2, sub7};

int run_slot(int which, int x)
{
    Op op = dispatch[which];
    return op(x);
}

int is_add10(Op op)
{
    return op == add10 && op == &add10;
}

int main(void)
{
    int acc = 0;

    acc += run_slot(0, 32);
    acc += run_slot(1, 21);
    acc += run_slot(2, 49);

    if (!is_add10(dispatch[0]))
    {
        return 1;
    }
    if (dispatch[1] != mul2)
    {
        return 2;
    }
    if ((*dispatch[2])(49) != 42)
    {
        return 3;
    }

    return acc / 3;
}