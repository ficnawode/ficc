enum Color
{
    RED,
    GREEN,
    BLUE
};

enum Mode
{
    OFF,
    ON = 5,
    AUTO
};

struct Pixel
{
    enum Color c;
    int value;
};

int main(void)
{
    if (sizeof(enum Color) != 4)
    {
        return 1;
    }
    if (sizeof(enum Mode) != 4)
    {
        return 2;
    }
    if (sizeof(struct Pixel) != 8)
    {
        return 3;
    }
    enum Color c = BLUE;
    enum Mode m = AUTO;
    struct Pixel p;
    p.c = GREEN;
    p.value = 5;
    return c + m + p.c * 10 + p.value;
}
