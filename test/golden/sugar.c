/* Phase 12: "sugar" — typedef + designated initializers + compound
   literals — end-to-end golden test.
   Compile with ficc (`ficc -run sugar.c` for the interpreter, `ficc -c` +
   `gcc -no-pie` for the ELF), and expect exit status 42.

   This exercises the three Phase-12 features together: ordinary-name
   typedefs (D12.1), the §6.7.9 cursor planner (designators, elision,
   char-array strings, array-length inference), and compound literals as
   anonymous static (file scope) / automatic (block scope) lvalues (D12.9),
   including addresses taken of those lvalues. */

typedef int Count;
typedef struct Point Point; /* incomplete record through a typedef name */

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

Point *units = &(Point){1, 2}; /* file-scope compound literal -> anonymous static */

int sum_pair(Point *p)
{
    return p->x + p->y;
}

int main(void)
{
    /* Incomplete-array completion from a designated initializer. */
    int elems[] = {[2] = 10, [4] = 7};
    if (sizeof(elems) != 20)
    {
        return 1;
    }
    if (elems[0] != 0 || elems[2] != 10 || elems[4] != 7)
    {
        return 2;
    }

    /* Block-scope compound literals: value, member access, address-in-arg. */
    if ((Point){3, 4}.y != 4)
    {
        return 3;
    }
    if (sum_pair(&(Point){10, 20}) != 30)
    {
        return 4;
    }

    /* The file-scope anonymous static survives both executions. */
    if (units->x != 1 || units->y != 2)
    {
        return 5;
    }

    /* Sew compound literals into a designated list via struct pointers. */
    Point pa = (Point){.y = 6, .x = 9};
    Point pb = (Point){.x = 1};
    struct Line ln = (struct Line){.a = &pa, .b = &pb};
    if (ln.a->x + ln.a->y + ln.b->x + ln.b->y != 16)
    {
        return 6;
    }

    /* typedef Count throughout, incl. array-length inference. */
    Count scores[] = {1, 2, 3};
    if (scores[0] + scores[1] + scores[2] != 6)
    {
        return 7;
    }

    return 42;
}