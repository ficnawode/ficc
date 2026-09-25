#include "harness.h"
#include "testdriver.h"

/* Backend-quality corpus: the shapes codegen changes (constant materialization,
   switch lowering, compares, spills) are most likely to miscompile. Every case
   asserts the interpreter and the linked ELF agree on the exit value. */

TEST(codegen_quality, compare_zero_signed)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void){ int a=-3, b=0, c=5;\n"
        "  return (a<0) + (b==0)*2 + (c>0)*4 + (c>=0)*8 + (a<=0)*16 + (b!=0)*32; }\n",
        31);
}

TEST(codegen_quality, compare_zero_unsigned)
{
    EXPECT_INTERP_AND_ELF("int main(void){ unsigned a=0, b=5;\n"
                          "  return (a==0) + (b>0)*2 + (a<=0)*4 + (b>=1)*8 + (a<1)*16; }\n",
                          31);
}

TEST(codegen_quality, compare_zero_narrow)
{
    EXPECT_INTERP_AND_ELF("int main(void){ char c=-1; short s=7; unsigned char u=200;\n"
                          "  return (c<0) + (s>0)*2 + (u>0)*4 + (c==0)*8 + (u==0)*16; }\n",
                          7);
}

TEST(codegen_quality, inc_dec_loops)
{
    EXPECT_INTERP_AND_ELF("int main(void){ int s=0;\n"
                          "  for (int i=0;i<10;i=i+1) { s=s+i; }\n"
                          "  for (int j=10;j>0;j=j-1) { s=s+1; }\n"
                          "  return s; }\n",
                          55);
}

TEST(codegen_quality, constant_materialization_boundaries)
{
    EXPECT_INTERP_AND_ELF("int main(void){ long v[5]; v[0]=-1; v[1]=2147483647; v[2]=2147483648L;\n"
                          "  v[3]=-2147483648L; v[4]=0; int s=0;\n"
                          "  for (int i=0;i<5;i=i+1) { s = s + (int)(v[i] % 13); }\n"
                          "  return s; }\n",
                          9);
}

TEST(codegen_quality, spills_and_compares)
{
    EXPECT_INTERP_AND_ELF(
        "int main(void){ int a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9,j=10,k=11,l=12;\n"
        "  int s=0;\n"
        "  if (a<b) s=s+1; if (c<d) s=s+2; if (e<f) s=s+4;\n"
        "  if (g<h) s=s+8; if (i<j) s=s+16; if (k<l) s=s+32;\n"
        "  return s; }\n",
        63);
}

TEST(codegen_quality, switch_dense_table)
{
    EXPECT_INTERP_AND_ELF("int main(void){ int s=0;\n"
                          "  for (int i=0;i<20;i=i+1) { switch (i%8) {\n"
                          "    case 0: s=s+1; break;   case 1: s=s+2; break;\n"
                          "    case 2: s=s+4; break;   case 3: s=s+8; break;\n"
                          "    case 4: s=s+16; break;  case 5: s=s+32; break;\n"
                          "    case 6: s=s+64; break;  default: s=s+128; break; } }\n"
                          "  return s%251; }\n",
                          23);
}

TEST(codegen_quality, switch_negative_range)
{
    EXPECT_INTERP_AND_ELF("int main(void){ int s=0;\n"
                          "  for (int i=-4;i<=4;i=i+1) { switch (i) {\n"
                          "    case -4: s=s+1; break;   case -3: s=s+2; break;\n"
                          "    case -2: s=s+4; break;   case -1: s=s+8; break;\n"
                          "    case 0: s=s+16; break;   case 1: s=s+32; break;\n"
                          "    case 2: s=s+64; break;   case 3: s=s+128; break;\n"
                          "    case 4: s=s+256; break;  default: s=s+512; break; } }\n"
                          "  return s%251; }\n",
                          9);
}

TEST(codegen_quality, switch_sparse_chain)
{
    EXPECT_INTERP_AND_ELF("int main(void){ int s=0;\n"
                          "  for (int i=0;i<100;i=i+1) { switch (i) {\n"
                          "    case 1: s=s+1; break; case 50: s=s+3; break;\n"
                          "    case 99: s=s+7; break; default: break; } }\n"
                          "  return s%251; }\n",
                          11);
}

TEST(codegen_quality, switch_wide_control_chain)
{
    EXPECT_INTERP_AND_ELF("int main(void){ long x=5000000000L; int s=0;\n"
                          "  switch (x) { case 5000000000L: s=s+42; break;\n"
                          "               default: s=s+1; break; }\n"
                          "  return s; }\n",
                          42);
}

TEST(codegen_quality, phi_copy_cycle)
{
    EXPECT_INTERP_AND_ELF("int main(void){ int a=1,b=2;\n"
                          "  for (int i=0;i<5;i=i+1) { int t=a; a=b; b=t+a; }\n"
                          "  return b%251; }\n",
                          21);
}

TEST(codegen_quality, call_arg_pressure)
{
    EXPECT_INTERP_AND_ELF("int id(int x){ return x; }\n"
                          "int main(void){ int a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8;\n"
                          "  return id(a)+id(b)+id(c)+id(d)+id(e)+id(f)+id(g)+id(h); }\n",
                          36);
}

TEST(codegen_quality, leaf_and_call_frames)
{
    EXPECT_INTERP_AND_ELF("int add(int a,int b){ return a+b; }\n"
                          "int leaf(int x){ return x*x; }\n"
                          "int main(void){ return (leaf(5)+add(10,7))%251; }\n",
                          42);
}

TEST(codegen_quality, long_double_compare)
{
    EXPECT_INTERP_AND_ELF("int main(void){ long double a=1.5L,b=2.5L; int s=0;\n"
                          "  if (a<b) s=s+1; if (b>a) s=s+2; if (a!=b) s=s+4; if (a==b) s=s+8;\n"
                          "  return s; }\n",
                          7);
}

TEST(codegen_quality, unsigned_long_shift)
{
    EXPECT_INTERP_AND_ELF("int main(void){ unsigned long x=0xFFFFFFFFFFFFFFFFUL;\n"
                          "  return (int)(x>>60); }\n",
                          15);
}

TEST(codegen_quality, gpr_values_split_across_a_call)
{
    EXPECT_INTERP_AND_ELF("volatile int src;\n"
                          "int f(int x,int y,int z,int w,int v){ return x+y+z+w+v; }\n"
                          "int main(void){ src=10;\n"
                          "  int a=src,b=src+1,c=src+2,d=src+3,e=src+4,g=src+5,h=src+6,i=src+7;\n"
                          "  int r=f(1,2,3,4,5);\n"
                          "  return (a+b+c+d+e+g+h+i+r)%251; }\n",
                          123);
}

TEST(codegen_quality, xmm_values_split_across_a_call)
{
    EXPECT_INTERP_AND_ELF("volatile double dsrc;\n"
                          "double f(double x){ return x+1.0; }\n"
                          "int main(void){ dsrc=2.5;\n"
                          "  double a=dsrc,b=dsrc+1.0,c=dsrc+2.0;\n"
                          "  double r=f(0.5);\n"
                          "  return (int)(a+b+c+r); }\n",
                          12);
}
