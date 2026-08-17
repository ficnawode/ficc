int main(void)
{
    char *s = "hello";
    int i = 0;
    while (s[i] != 0)
    {
        i = i + 1;
    }
    return i; /* length of "hello" = 5 */
}
