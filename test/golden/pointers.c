int sum(int a[], int n)
{
    int s = 0;
    int i;
    for (i = 0; i < n; i = i + 1)
    {
        s = s + a[i];
    }
    return s;
}

int main(void)
{
    int arr[4];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    int *p = &arr[1];
    int deref = *p + *(p - 1) + *(p + 2);
    return sum(arr, 4) + deref; /* 100 + 70 = 170 */
}
