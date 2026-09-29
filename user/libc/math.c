#include <math.h>
#include <stdint.h>

/* Rounding to an integer: SSE4.1's roundsd isn't in baseline x86-64, so
 * go through the x87 with the rounding-control field set for the call. */
static double round_with(double x, uint16_t rc) {
    uint16_t cw, tmp;
    asm volatile("fnstcw %0" : "=m"(cw));
    tmp = (uint16_t)((cw & ~0x0C00) | rc);
    asm volatile("fldcw %0" : : "m"(tmp));
    asm volatile("frndint" : "+t"(x));
    asm volatile("fldcw %0" : : "m"(cw));
    return x;
}

double fabs(double x) {
    union { double d; uint64_t u; } v = {x};
    v.u &= ~(1ULL << 63);
    return v.d;
}

double sqrt(double x) {
    asm("sqrtsd %1, %0" : "=x"(x) : "x"(x));
    return x;
}

double floor(double x) { return round_with(x, 0x0400); }
double ceil(double x) { return round_with(x, 0x0800); }
double trunc(double x) { return round_with(x, 0x0C00); }

double round(double x) {
    /* Halfway cases away from zero, unlike frndint's default to-even. */
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x < 0 ? -1 : 1;
    return t;
}

double fmod(double x, double y) {
    /* fprem gives a partial remainder; loop until C2 says it's complete. */
    uint16_t sw;
    do {
        asm("fprem; fnstsw %1" : "+t"(x), "=a"(sw) : "u"(y));
    } while (sw & 0x0400);
    return x;
}

/* log2(x) = fyl2x with y = 1. */
double log2(double x) {
    double r;
    asm("fld1; fxch; fyl2x" : "=t"(r) : "0"(x));
    return r;
}

double log(double x) { return log2(x) * 0.69314718055994530942; }
double log10(double x) { return log2(x) * 0.30102999566398119521; }

/* 2^x: split into integer and fraction; f2xm1 handles |f| <= 1, fscale
 * applies the integer part. */
static double exp2_x87(double x) {
    if (isnan(x)) return x;
    if (x > 1100) return INFINITY;
    if (x < -1100) return 0;
    double i = round_with(x, 0x0000), f = x - i, r;
    asm("f2xm1; fld1; faddp; fscale; fstp %%st(1)" : "=t"(r) : "0"(f), "u"(i) : "st(1)");
    return r;
}

double exp(double x) { return exp2_x87(x * 1.44269504088896340736); }

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : INFINITY;
    if (x < 0) {
        /* Only integer exponents have a real result. */
        if (trunc(y) != y) return NAN;
        double r = exp2_x87(y * log2(-x));
        return fmod(y, 2) != 0 ? -r : r;
    }
    return exp2_x87(y * log2(x));
}

/* fsin/fcos/fptan only take |x| < 2^63; reduce by 2pi first so large
 * arguments still work (with the precision loss that implies). */
static double reduce(double x) {
    if (fabs(x) > 1e9) x = fmod(x, 2 * M_PI);
    return x;
}

double sin(double x) {
    x = reduce(x);
    asm("fsin" : "+t"(x));
    return x;
}

double cos(double x) {
    x = reduce(x);
    asm("fcos" : "+t"(x));
    return x;
}

double tan(double x) {
    x = reduce(x);
    asm("fptan; fstp %%st(0)" : "+t"(x));
    return x;
}

double atan2(double y, double x) {
    double r;
    asm("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan(double x) { return atan2(x, 1); }
double asin(double x) { return fabs(x) > 1 ? NAN : atan2(x, sqrt(1 - x * x)); }
double acos(double x) { return fabs(x) > 1 ? NAN : atan2(sqrt(1 - x * x), x); }
