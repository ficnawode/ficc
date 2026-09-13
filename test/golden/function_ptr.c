/* Phase 16: function pointers + indirect calls — end-to-end golden. Compile
   with ficc (`ficc -run function_ptr.c` for the interpreter, `ficc -c` +
   `gcc -no-pie` for the ELF), and expect exit status 42.

   Exercises the Phase 16b/16c surface together: typedef'd function-pointer
   types, a statically-initialized dispatch *table* of pointers to distinct
   functions, indirect calls through a table slot selected at runtime (the
   ficc `lower_fns` dispatch pattern), redundant `&f`/`f` equality, and
   passing/receiving function pointers across calls. */

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

    /* 32 -> add10 -> 42 */
    acc += run_slot(0, 32);
    /* 21 -> mul2 -> 42; 49 -> sub7 -> 42, and a redundant indirect call */
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

    /* acc = 42 + 42 + 42 = 126 = 3 * 42; scale to 42. */
    return acc / 3;
}