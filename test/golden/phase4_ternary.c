int main(void) {
    int a = 5;
    int b = 3;
    int c = a > b ? 7 : 2;
    int d = a == b ? 100 : a < b ? 200 : 300;
    int e = (a && b) ? c : (c + d);
    return e;
}
