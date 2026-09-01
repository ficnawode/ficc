int main(void)
{
    int s = 0;
    int i = 0;
    while (i < 5)
    {
        s = s + i;
        i = i + 1;
    }
    for (i = 0; i < 3; i = i + 1)
    {
        s = s + i;
    }
    do
    {
        s = s - 1;
    } while (s > 10);
    return s;
}
