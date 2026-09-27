int add_one(int n)
{
    {
        int n = 100;
        n = n + 1;
    }
    return n;
}

int main(void)
{
    int x = 1;
    {
        int x = 2;
        x = x + 3;
    }
    int y = 0;
    if (x)
    {
        int y = 10;
        y = y + 1;
    }
    else
    {
        int y = 20;
        y = y + 1;
    }
    return x + y + add_one(5);
}
