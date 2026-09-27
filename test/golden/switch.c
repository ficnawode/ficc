/* Expect exit 42. */

int tab(int v)
{
    switch (v)
    {
        case 0:
            return 0;
        case 1:
            return 1;
        case 2:
            return 2;
        case 3:
            return 3;
        default:
            return 9;
    }
}

int sgn(int v)
{
    switch (v)
    {
        case -2:
            return -2;
        case 0:
            return 0;
        case 2:
            return 2;
        default:
            return 99;
    }
}

int far(int v)
{
    switch (v)
    {
        case 1000000:
            return 7;
        case 1000000000:
            return 6;
        default:
            return 3;
    }
}

int mode(int m)
{
    switch (m)
    {
        case 1:
        case 2:
            return 10;
        case 3:
            return 20;
        default:
            return 30;
    }
}

/* Nested switch: an inner dense table switch inside an outer one. The inner
   switch is free to reuse the outer's case values (C11 §6.8.4.2p3). */
int nest(int outer, int inner)
{
    switch (outer)
    {
        case 0:
            return 100;
        case 1:
            switch (inner)
            {
                case 0:
                    return 1;
                case 1:
                case 2:
                    return 2;
                default:
                    return 3;
            }
        case 2:
            return 200;
        default:
            return 999;
    }
}

int sum(int n)
{
    int s = 0;
    int i = 0;
    while (i < n)
    {
        switch (i % 4)
        {
            case 0:
                s = s + 1;
                break;
            case 1:
                s = s + 2;
                break;
            case 2:
            case 3:
                s = s + 3;
                break;
        }
        i = i + 1;
    }
    return s;
}

int main(void)
{
    if (tab(0) != 0)
    {
        return 1;
    }
    if (tab(3) != 3)
    {
        return 2;
    }
    if (tab(4) != 9)
    {
        return 3;
    }
    if (sgn(-2) != -2)
    {
        return 4;
    }
    if (sgn(-1) != 99)
    {
        return 5;
    }
    if (sgn(0) != 0)
    {
        return 6;
    }
    if (sgn(2) != 2)
    {
        return 7;
    }
    if (sgn(5) != 99)
    {
        return 8;
    }
    if (far(1000000) != 7)
    {
        return 9;
    }
    if (far(1000000000) != 6)
    {
        return 10;
    }
    if (far(1) != 3)
    {
        return 11;
    }
    if (mode(1) != 10)
    {
        return 12;
    }
    if (mode(2) != 10)
    {
        return 13;
    }
    if (mode(3) != 20)
    {
        return 14;
    }
    if (mode(9) != 30)
    {
        return 15;
    }
    if (sum(5) != 10)
    {
        return 16;
    }
    if (sum(8) != 18)
    {
        return 17;
    }
    if (nest(1, 0) != 1)
    {
        return 18;
    }
    if (nest(1, 2) != 2)
    {
        return 19;
    }
    if (nest(1, 9) != 3)
    {
        return 20;
    }
    if (nest(0, 5) != 100)
    {
        return 21;
    }
    if (nest(2, 5) != 200)
    {
        return 22;
    }
    if (nest(7, 1) != 999)
    {
        return 23;
    }
    return 42;
}