/* px_dtoa.c: exact number <-> decimal text, without printf. Used by the
 * lexer as well as the engine, so it depends on nothing else. */

#ifndef PX_DTOA_H
#define PX_DTOA_H

#include <stddef.h>

int    px_dtoa_shortest(double v, char *digits, int *k);                  /* v > 0: v ~ 0.digits * 10^k */
int    px_dtoa_exact(double v, int ndigits, int frac, char *digits, int *k); /* rounded half up */
size_t px_fmt_number(double d, char *out, size_t cap);                     /* Number::toString, finite d */
size_t px_fmt_fixed(double d, int f, char *out, size_t cap);               /* toFixed, |d| < 1e21 */
size_t px_fmt_exponential(double d, int f, char *out, size_t cap);         /* f < 0: shortest */
size_t px_fmt_precision(double d, int p, char *out, size_t cap);
double px_decimal_to_double(const void *text, int wide, size_t len); /* a checked decimal literal */
double px_radix2_to_double(const void *text, int wide, size_t len, int shift); /* hex (4), octal (3), binary (1) */

#endif
