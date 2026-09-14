#ifndef FICC_MATH_H
#define FICC_MATH_H

/* IEEE-754 constants for the host (x86-64, binary32/64/80).
   HUGE_VAL must be a value that overflows to +infinity (POSIX/C11
   requires the *concept* of overflow, and code tests `inf == HUGE_VAL`,
   e.g. Lua's l_hashfloat); DBL_MAX would fail such tests. */
#define HUGE_VAL 1e10000
#define HUGE_VALF 1e10000f
#define HUGE_VALL 1e10000L

#define INFINITY (1.0 / 0.0)
#define NAN (0.0 / 0.0)

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double ceil(double x);
double cos(double x);
double cosh(double x);
double exp(double x);
double fabs(double x);
double floor(double x);
double fmod(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double log(double x);
double log10(double x);
double log2(double x);
double modf(double x, double *iptr);
double pow(double x, double y);
double sin(double x);
double sinh(double x);
double sqrt(double x);
double tan(double x);
double tanh(double x);
double remainder(double x, double y);
double trunc(double x);
double round(double x);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
double hypot(double x, double y);
double cbrt(double x);

float acosf(float x);
float asinf(float x);
float atanf(float x);
float atan2f(float y, float x);
float ceilf(float x);
float cosf(float x);
float coshf(float x);
float expf(float x);
float fabsf(float x);
float floorf(float x);
float fmodf(float x, float y);
float frexpf(float x, int *exp);
float ldexpf(float x, int exp);
float logf(float x);
float log10f(float x);
float log2f(float x);
float modff(float x, float *iptr);
float powf(float x, float y);
float sinf(float x);
float sinhf(float x);
float sqrtf(float x);
float tanf(float x);
float tanhf(float x);

#define isinf(x) ((x) == HUGE_VAL || (x) == -HUGE_VAL)
#define isnan(x) ((x) != (x))
#define isfinite(x) (!isnan(x) && !isinf(x))
#define signbit(x) ((x) < 0 || ((x) != (x) && 0))

#endif