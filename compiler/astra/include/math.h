#ifndef _ASTRA_MATH_H
#define _ASTRA_MATH_H

#include <stdint.h>
#include <float.h>

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_NORMAL 2
#define FP_SUBNORMAL 3
#define FP_ZERO 4

static inline uint64_t a_dbl_bits(double x) {
    union { double d; uint64_t u; } v;
    v.d = x;
    return v.u;
}

static inline double a_inf_d(void) {
    union { double d; uint64_t u; } v;
    v.u = 0x7FF0000000000000ULL;
    return v.d;
}

static inline double a_nan_d(void) {
    union { double d; uint64_t u; } v;
    v.u = 0x7FF8000000000000ULL;
    return v.d;
}

#define HUGE_VAL (a_inf_d())
#define HUGE_VALF (a_inf_d())
#define HUGE_VALL (a_inf_d())
#define NAN (a_nan_d())
#define INFINITY (a_inf_d())

double sqrt(double x);
double cbrt(double x);
double pow(double x, double y);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);

double fabs(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double copysign(double x, double y);
double fdim(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);
double hypot(double x, double y);
double fma(double x, double y, double z);

double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double nearbyint(double x);
double rint(double x);
long lrint(double x);
long long llrint(double x);
long lround(double x);
long long llround(double x);

static inline int a_fpclassify_d(double x) {
    union { double d; uint64_t u; } v;
    v.d = x;
    {
        uint64_t e = (v.u >> 52) & 0x7FFULL;
        uint64_t m = v.u & 0x000FFFFFFFFFFFFFULL;
        if (e == 0x7FFULL) return m ? FP_NAN : FP_INFINITE;
        if (e == 0) return m ? FP_SUBNORMAL : FP_ZERO;
        return FP_NORMAL;
    }
}

#define fpclassify(x) a_fpclassify_d(x)
#define isfinite(x) (fpclassify(x) >= FP_NORMAL)
#define isinf(x) (fpclassify(x) == FP_INFINITE)
#define isnan(x) (fpclassify(x) == FP_NAN)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x) (a_dbl_bits(x) >> 63)

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double scalbn(double x, int n);
double scalbln(double x, long n);

float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float powf(float x, float y);
float fmodf(float x, float y);
float expf(float x);
float logf(float x);

long double sqrtl(long double x);
long double fabsl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double roundl(long double x);
long double truncl(long double x);
long double powl(long double x, long double y);
long double fmodl(long double x, long double y);
long double expl(long double x);
long double logl(long double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

#endif /* _ASTRA_MATH_H */
