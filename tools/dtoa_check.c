/* dtoa_check: px_dtoa.c on the lines of stdin, for tools/dtoa_check.py.
 *
 *   s <hex bits>           Number::toString
 *   f <digits> <hex bits>  toFixed
 *   e <digits> <hex bits>  toExponential (-1: no argument)
 *   p <digits> <hex bits>  toPrecision
 *   d <text>               decimal -> double, printed as hex bits
 *
 * Built by the check script: gcc -Iinclude -Isrc tools/dtoa_check.c src/px_dtoa.c -lm */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px_dtoa.h"

int main(void) {
    char line[4096], out[256];
    while (fgets(line, sizeof line, stdin)) {
        char              op = line[0];
        int               arg = 0;
        unsigned long long bits;
        double            d;
        line[strcspn(line, "\r\n")] = '\0';
        if (op == 'd') {
            d = px_decimal_to_double(line + 2, 0, strlen(line + 2));
            memcpy(&bits, &d, 8);
            printf("%016llx\n", bits);
            continue;
        }
        if (op == 's') sscanf(line + 2, "%llx", &bits);
        else sscanf(line + 2, "%d %llx", &arg, &bits);
        memcpy(&d, &bits, 8);
        switch (op) {
        case 's': px_fmt_number(d, out, sizeof out); break;
        case 'f': px_fmt_fixed(d, arg, out, sizeof out); break;
        case 'e': px_fmt_exponential(d, arg, out, sizeof out); break;
        case 'p': px_fmt_precision(d, arg, out, sizeof out); break;
        default: strcpy(out, "?");
        }
        puts(out);
    }
    return 0;
}
