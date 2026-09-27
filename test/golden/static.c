int counter(void)
{
    static int n = 5;
    n = n + 1;
    return n;
}

int other(void)
{
    static int n;
    n = n + 2;
    return n;
}

int main(void)
{
    int a = counter();
    int b = counter();
    int c = other();
    int d = other();
    return a + b + c + d;
}
