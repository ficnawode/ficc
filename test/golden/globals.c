static int accumulator;
int counter = 5;
int arr[3];
char *greeting = "hi";

int main(void)
{
    counter = counter + 2;
    accumulator = accumulator + counter;
    arr[1] = 6;
    accumulator = accumulator + arr[1] + greeting[1];
    return accumulator;
}
