/*
 * aether_jq.c — the two things contrib.jq cannot do from Aether alone.
 *
 *   1. Shortest round-trip number formatting. jq prints 0.1 + 0.2 as
 *      0.30000000000000004 and 1/3 as 0.3333333333333333: the fewest
 *      significant digits that read back as the same double. The runtime's
 *      string_from_double is a fixed %.17g (0.33333333333333331), so the
 *      "try 15, 16, then 17 digits" probe lives here, on the locale-pinned
 *      snprintf/strtod pair every other number path in the runtime uses.
 *
 *   2. The process environment, for jq's `$ENV` / `env`. std.os offers
 *      getenv(name) but no way to enumerate; `environ` is the only door.
 *
 * Everything else — lexer, parser, evaluator, builtins — is Aether.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../std/string/aether_string.h"
#include "../../runtime/aether_locale_num.h"

extern char** environ;

/* Format `d` with the fewest significant digits (15..17) that round-trip.
 * Integral values below 1e19 print as plain integers, the way jq 1.7 and
 * gojq print 1e17 (100000000000000000, not 1e+17). Infinities clamp to the
 * largest finite double, which is what jq prints for `infinite`; NaN is the
 * caller's problem (it serialises as null). */
AetherString* aether_jq_num_to_str(double d) {
    char buf[64];
    if (isnan(d)) return string_new("null");
    if (isinf(d)) d = d < 0 ? -1.7976931348623157e+308 : 1.7976931348623157e+308;
    if (d == 0) return string_new(signbit(d) ? "-0" : "0");

    /* The shortest %e rendering (15..17 significant digits) that reads back
     * as the same double gives the digit string and decimal exponent. */
    char tmp[64];
    int prec;
    for (prec = 15; prec <= 17; prec++) {
        aether_c_snprintf_double(tmp, sizeof tmp, prec == 15 ? "%.14e" : prec == 16 ? "%.15e" : "%.16e", d);
        char* end = NULL;
        if (aether_c_strtod(tmp, &end) == d) break;
    }
    const char* q = tmp;
    int neg = 0;
    if (*q == '-') { neg = 1; q++; }
    char digs[32];
    int nd = 0;
    while (*q && *q != 'e' && *q != 'E') {
        if (*q >= '0' && *q <= '9') digs[nd++] = *q;
        q++;
    }
    int exp10 = (*q == 'e' || *q == 'E') ? atoi(q + 1) : 0;
    while (nd > 1 && digs[nd - 1] == '0') nd--;
    digs[nd] = 0;

    /* Layout follows gojq (and JSON.stringify): plain digits between 1e-6
     * and 1e21, exponent notation outside, with no zero-padded exponent. */
    char* o = buf;
    if (neg) *o++ = '-';
    double a = fabs(d);
    if (a < 1e-6 || a >= 1e21) {
        *o++ = digs[0];
        if (nd > 1) { *o++ = '.'; memcpy(o, digs + 1, nd - 1); o += nd - 1; }
        o += sprintf(o, "e%c%d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
    } else if (exp10 >= nd - 1) {
        memcpy(o, digs, nd); o += nd;
        for (int i = 0; i < exp10 - (nd - 1); i++) *o++ = '0';
    } else if (exp10 >= 0) {
        memcpy(o, digs, exp10 + 1); o += exp10 + 1;
        *o++ = '.';
        memcpy(o, digs + exp10 + 1, nd - exp10 - 1); o += nd - exp10 - 1;
    } else {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -exp10 - 1; i++) *o++ = '0';
        memcpy(o, digs, nd); o += nd;
    }
    *o = 0;
    return string_new(buf);
}

int aether_jq_environ_count(void) {
    int n = 0;
    if (!environ) return 0;
    while (environ[n]) n++;
    return n;
}

/* The i-th "KEY=VALUE" entry, or "" past the end. */
AetherString* aether_jq_environ_get(int i) {
    if (!environ || i < 0) return string_new("");
    int n = 0;
    while (environ[n]) n++;
    if (i >= n) return string_new("");
    return string_new(environ[i]);
}

/* The libm functions jq exposes that std.math does not wrap. One entry
 * point per arity keeps the extern surface small; the op numbers are the
 * MATH1_* / MATH2_* constants in eval.ae. */
double aether_jq_math1(int op, double x) {
    switch (op) {
    case 0: return floor(x);
    case 1: return ceil(x);
    case 2: return round(x);
    case 3: return trunc(x);
    case 4: return rint(x);
    case 5: return fabs(x);
    case 6: return sqrt(x);
    case 7: return cbrt(x);
    case 8: return exp(x);
    case 9: return exp2(x);
    case 10: return pow(10.0, x);
    case 11: return expm1(x);
    case 12: return log(x);
    case 13: return log2(x);
    case 14: return log10(x);
    case 15: return log1p(x);
    case 16: return sin(x);
    case 17: return cos(x);
    case 18: return tan(x);
    case 19: return asin(x);
    case 20: return acos(x);
    case 21: return atan(x);
    case 22: return sinh(x);
    case 23: return cosh(x);
    case 24: return tanh(x);
    case 25: return asinh(x);
    case 26: return acosh(x);
    case 27: return atanh(x);
    case 28: return tgamma(x);
    case 29: return lgamma(x);
    case 30: return log(fabs(tgamma(x)));   /* lgamma_r, sign dropped */
    case 31: return nearbyint(x);
    case 32: return (double)(int)(logb(x));
    case 33: return frexp(x, &(int){0});
    default: return x;
    }
}

double aether_jq_math2(int op, double x, double y) {
    switch (op) {
    case 0: return pow(x, y);
    case 1: return atan2(x, y);
    case 2: return fmod(x, y);
    case 3: return fmin(x, y);
    case 4: return fmax(x, y);
    case 5: return fdim(x, y);
    case 6: return hypot(x, y);
    case 7: return copysign(x, y);
    case 8: return nextafter(x, y);
    case 9: return ldexp(x, (int)y);
    case 10: return scalbn(x, (int)y);
    case 11: return remainder(x, y);
    default: return x;
    }
}

double aether_jq_math3(int op, double x, double y, double z) {
    switch (op) {
    case 0: return fma(x, y, z);
    default: return x;
    }
}

/* frexp's exponent, for jq's `frexp` which returns [mantissa, exponent]. */
int aether_jq_frexp_exp(double x) {
    int e = 0;
    frexp(x, &e);
    return e;
}

/* modf's integral part; the fraction is x minus it. */
double aether_jq_modf_int(double x) {
    double ip = 0.0;
    modf(x, &ip);
    return ip;
}
