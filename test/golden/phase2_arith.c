int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

int main(void) {
    int x;
    x = add(10, 3);
    x = sub(x, 2);
    return x * 5;   /* (10+3-2)*5 = 55 */
}
