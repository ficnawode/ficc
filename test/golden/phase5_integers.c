int add_char(char a, char b) { return a + b; }
int add_short(short a, short b) { return a + b; }
int add_uchar(unsigned char a, unsigned char b) { return a + b; }
int add_ushort(unsigned short a, unsigned short b) { return a + b; }
int add_uint(unsigned a, unsigned b) { return a + b; }

int cmp_unsigned(unsigned a, unsigned b) { return a < b; }
int cmp_signed_lt(int a, int b) { return a < b; }
int cmp_unsigned_gt(unsigned a, unsigned b) { return a > b; }

int shift_left(int a, int b) { return a << b; }
int shift_right_signed(int a, int b) { return a >> b; }
unsigned shift_right_unsigned(unsigned a, unsigned b) { return a >> b; }

int trunc_char(void) { char x = 300; return x; }
int trunc_short(void) { short x = 70000; return x; }
int trunc_uchar(void) { unsigned char x = 300; return x; }

int main(void) {
    int r = 0;
    r = r + add_char(100, 50);
    r = r + add_short(1000, 2000);
    r = r + add_uchar(200, 50);
    r = r + add_ushort(10000, 20000);
    r = r + add_uint(100000, 200000);
    r = r + cmp_unsigned(5, 10);
    r = r + cmp_signed_lt(-5, 10);
    r = r + cmp_unsigned_gt(20, 10);
    r = r + shift_left(1, 3);
    r = r + shift_right_signed(-8, 1);
    r = r + shift_right_unsigned(8, 1);
    r = r + trunc_char();
    r = r + trunc_short();
    r = r + trunc_uchar();
    return r;
}
