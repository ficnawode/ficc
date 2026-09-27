/* Expect exit 42. */

typedef int Count;
typedef struct Point Point;

struct Point
{
    int x;
    int y;
};

struct Line
{
    Point *a;
    Point *b;
};

Point *units = &(Point) {1, 2};

int sum_pair(Point *p)
{
    return p->x + p->y;
}

int main(void)
{
    int elems[] = {[2] = 10, [4] = 7};
    if (sizeof(elems) != 20)
    {
        return 1;
    }
    if (elems[0] != 0 || elems[2] != 10 || elems[4] != 7)
    {
        return 2;
    }

    if ((Point) {3, 4}.y != 4)
    {
        return 3;
    }
    if (sum_pair(&(Point) {10, 20}) != 30)
    {
        return 4;
    }

    if (units->x != 1 || units->y != 2)
    {
        return 5;
    }

    Point pa = (Point) {.y = 6, .x = 9};
    Point pb = (Point) {.x = 1};
    struct Line ln = (struct Line) {.a = &pa, .b = &pb};
    if (ln.a->x + ln.a->y + ln.b->x + ln.b->y != 16)
    {
        return 6;
    }

    Count scores[] = {1, 2, 3};
    if (scores[0] + scores[1] + scores[2] != 6)
    {
        return 7;
    }

    return 42;
}