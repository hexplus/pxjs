/* Date: milliseconds since 1970-01-01 UTC in a double, and the civil
 * calendar arithmetic around it (proleptic Gregorian, Howard Hinnant's
 * days-from-civil algorithms: exact, no tables, no loops).
 *
 * LOCAL TIME is what the C library's localtime() says. On the PSP that is
 * the console's own time zone and daylight-saving setting (PSPSDK's libc
 * glue sets TZ from them); on a host it is the TZ environment variable
 * (UTC when unset, as in the test container). Outside the years a 32-bit
 * time_t covers (1901-2038) the offset at the nearer end of that range
 * applies, so every platform gives the same answer for far dates whatever
 * its time_t: no daylight saving there, the zone's standard offset. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "px_internal.h"

#if defined(__psp__)
#include <psprtc.h>
#endif

#define ARG(i) px_arg(argc, argv, (i))
#define MAGIC  (vm->native_magic)

#define MS_PER_DAY 86400000.0
#define MAX_TIME   8.64e15

static const char *const k_days[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static double days_from_civil(double y, double m, double d) {
    /* m is 1..12 */
    double era, yoe, doy, doe;
    y -= m <= 2;
    era = floor(y / 400);
    yoe = y - era * 400;
    doy = floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    doe = yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(double z, int *y, int *m, int *d) {
    double era, doe, yoe, doy, mp;
    z += 719468;
    era = floor(z / 146097);
    doe = z - era * 146097;
    yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
    doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
    mp  = floor((5 * doy + 2) / 153);
    *d  = (int)(doy - floor((153 * mp + 2) / 5) + 1);
    *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y  = (int)(yoe + era * 400 + (*m <= 2));
}

/* MakeDay(year, month 0-based and overflowing, date) */
static double make_day(double y, double mon, double d) {
    double ym;
    if (!isfinite(y) || !isfinite(mon) || !isfinite(d)) return NAN;
    y   = trunc(y);
    mon = trunc(mon);
    ym  = y + floor(mon / 12);
    return days_from_civil(ym, mon - floor(mon / 12) * 12 + 1, 1) + trunc(d) - 1;
}

/* MakeTime, in the spec's order of operations (the rounding shows) */
static double make_time(double h, double mi, double s, double ms) {
    if (!isfinite(h) || !isfinite(mi) || !isfinite(s) || !isfinite(ms)) return NAN;
    return ((trunc(h) * 3600000.0 + trunc(mi) * 60000.0) + trunc(s) * 1000.0) + trunc(ms);
}

/* MakeDate over the seven constructor fields: year, month, date, hours,
 * minutes, seconds, ms */
static double make_date(const double *c) {
    double day = make_day(c[0], c[1], c[2]), time = make_time(c[3], c[4], c[5], c[6]);
    if (!isfinite(day) || !isfinite(time)) return NAN;
    return day * MS_PER_DAY + time;
}

static double time_clip(double t) {
    if (!isfinite(t) || fabs(t) > MAX_TIME) return NAN;
    return trunc(t) + 0; /* +0 turns -0 into 0 */
}

/* Date(y, ...) and Date.UTC: a year from 0 to 99 means 1900 to 1999 */
static double full_year(double y) {
    double yi = trunc(y);
    return !isnan(y) && yi >= 0 && yi <= 99 ? 1900 + yi : y;
}

/* The local time offset from UTC in ms at UTC time t (DST aware); see the
 * top of the file. */
static double tz_offset(double t) {
    double    s = floor(t / 1000);
    time_t    tt;
    struct tm lt, gt;
    double    lday, gday;
    if (!isfinite(t)) return 0;
    tt = (time_t)(s < -2147483648.0 ? -2147483648.0 : s > 2147483647.0 ? 2147483647.0 : s);
#if defined(_WIN32)
    {
        struct tm *p = localtime(&tt), *q;
        if (!p) return 0;
        lt = *p;
        q  = gmtime(&tt);
        if (!q) return 0;
        gt = *q;
    }
#else
    if (!localtime_r(&tt, &lt) || !gmtime_r(&tt, &gt)) return 0;
#endif
    lday = days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    gday = days_from_civil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday);
    return ((lday - gday) * 86400.0 + (lt.tm_hour - gt.tm_hour) * 3600.0 + (lt.tm_min - gt.tm_min) * 60.0 +
            (lt.tm_sec - gt.tm_sec)) *
           1000.0;
}

/* UTC(t) for a local time t. Near a daylight-saving switch a local time
 * can name two instants (the clock went back: the earlier one counts) or
 * none (it went forward: read it with the offset from before). */
static double local_to_utc(double t) {
    double before, after, u, v;
    if (!isfinite(t)) return NAN;
    before = tz_offset(t - MS_PER_DAY);
    after  = tz_offset(t + MS_PER_DAY);
    if (before == after) return t - before;
    u = t - before;
    v = t - after;
    if (tz_offset(u) == before) return tz_offset(v) == after && v < u ? v : u;
    return tz_offset(v) == after ? v : u;
}

typedef struct Fields {
    int    year, month, day, weekday, hour, minute, second, ms;
    double tz; /* offset used, ms */
} Fields;

static void split(double t, int local, Fields *f) {
    double days, rem;
    f->tz = local ? tz_offset(t) : 0;
    t += f->tz;
    days = floor(t / MS_PER_DAY);
    rem  = t - days * MS_PER_DAY;
    civil_from_days(days, &f->year, &f->month, &f->day);
    f->month--;
    f->weekday = (int)fmod(days + 4, 7);
    if (f->weekday < 0) f->weekday += 7;
    f->hour   = (int)(rem / 3600000.0);
    f->minute = (int)(fmod(rem, 3600000.0) / 60000.0);
    f->second = (int)(fmod(rem, 60000.0) / 1000.0);
    f->ms     = (int)fmod(rem, 1000.0);
}

static double now_ms(void) {
    struct timeval tv;
#if defined(__psp__)
    /* Not gettimeofday there: on a PSP-1000 (6.61) it counted from about
     * the boot, and Date.now() came out in 1970. The RTC tick is UTC, in
     * microseconds since 0001-01-01. */
    u64 tick;
    if (sceRtcGetCurrentTick(&tick) >= 0) return (double)(tick / 1000u) - 62135596800000.0;
#endif
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

/* ------------------------------------------------------------ parsing */

/* Exactly n digits */
static int digits(const char **p, int n, int *out) {
    int i, v = 0;
    for (i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    *p += n;
    *out = v;
    return 1;
}

/* 1 to 9 digits */
static int number(const char **p, int *out) {
    int n = 0, v = 0;
    while (**p >= '0' && **p <= '9' && n < 9) {
        v = v * 10 + (*(*p)++ - '0');
        n++;
    }
    *out = v;
    return n > 0;
}

static void skip_spaces(const char **p) {
    while (**p == ' ') (*p)++;
}

/* A month or weekday name's three letters, any case: its index, or -1 */
static int name_index(const char **p, const char *const *names, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (((*p)[0] | 0x20) == (names[i][0] | 0x20) && ((*p)[1] | 0x20) == names[i][1] &&
            ((*p)[2] | 0x20) == names[i][2]) {
            *p += 3;
            return i;
        }
    return -1;
}

/* ":HH:mm" style offset (colon optional) after its sign; *off in ms */
static int offset(const char **p, double *off) {
    int sign = **p == '-' ? -1 : 1, h, m = 0;
    if (**p != '+' && **p != '-') return 0;
    (*p)++;
    if (!digits(p, 2, &h)) return 0;
    if (**p == ':') (*p)++;
    if (!digits(p, 2, &m)) return 0;
    if (h > 23 || m > 59) return 0;
    *off = sign * (h * 60 + m) * 60000.0;
    return 1;
}

static int days_in_month(int y, int m) {
    static const uint8_t k[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : k[m];
}

/* HH:mm[:ss[.sss]] into c[3..6]; 24:00 only as the end of a day */
static int clock_time(const char **p, double *c) {
    int h, mi, s = 0, ms = 0;
    if (!digits(p, 2, &h) || **p != ':') return 0;
    (*p)++;
    if (!digits(p, 2, &mi)) return 0;
    if (**p == ':') {
        (*p)++;
        if (!digits(p, 2, &s)) return 0;
        if (**p == '.' || **p == ',') {
            int k = 0, scale = 100;
            (*p)++;
            for (; **p >= '0' && **p <= '9'; (*p)++, k++, scale /= 10)
                if (k < 3) ms += (**p - '0') * scale;
            if (!k) return 0;
        }
    }
    if (h > 24 || mi > 59 || s > 59 || (h == 24 && (mi || s || ms))) return 0;
    c[3] = h;
    c[4] = mi;
    c[5] = s;
    c[6] = ms;
    return 1;
}

/* The Date Time String Format (21.4.1.32): YYYY[-MM[-DD]][THH:mm[:ss[.sss]]][Z|+HH:mm]
 * with an expanded year +YYYYYY/-YYYYYY; date-only forms are UTC,
 * date-times without an offset local. A space for the T is accepted too. */
static double parse_iso(const char *p) {
    double c[7] = {0, 0, 1, 0, 0, 0, 0}, off = 0;
    int    y, mo = 1, d = 1, has_time = 0, has_off = 0;
    if (*p == '+' || *p == '-') {
        int neg = *p++ == '-';
        if (!digits(&p, 6, &y) || (neg && y == 0)) return NAN; /* -000000 is not a year */
        if (neg) y = -y;
    } else if (!digits(&p, 4, &y)) {
        return NAN;
    }
    if (*p == '-') {
        p++;
        if (!digits(&p, 2, &mo)) return NAN;
        if (*p == '-') {
            p++;
            if (!digits(&p, 2, &d)) return NAN;
        }
    }
    if (mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo - 1)) return NAN;
    if (*p == 'T' || *p == 't' || (*p == ' ' && p[1] >= '0' && p[1] <= '9')) {
        p++;
        if (!clock_time(&p, c)) return NAN;
        has_time = 1;
        if (*p == 'Z' || *p == 'z') {
            p++;
            has_off = 1;
        } else if (*p == '+' || *p == '-') {
            if (!offset(&p, &off)) return NAN;
            has_off = 1;
        }
    }
    if (*p) return NAN;
    c[0] = y;
    c[1] = mo - 1;
    c[2] = d;
    if (!has_time || has_off) return time_clip(make_date(c) - off);
    return time_clip(local_to_utc(make_date(c)));
}


/* What toString and toUTCString write, for Date.parse to read back:
 *   [Www ]Mmm DD YYYY[ HH:mm[:ss]][ GMT+HHMM][ (zone name)]
 *   [Www, ]DD Mmm YYYY[ HH:mm[:ss]][ GMT]
 * (the year may be negative); without a zone, local time. */
static double parse_legacy(const char *p) {
    double c[7] = {0, 0, 1, 0, 0, 0, 0}, off = 0;
    int    mo, d, y, h, mi, s = 0, neg, local = 1;
    if (name_index(&p, k_days, 7) >= 0) {
        if (*p == ',') p++;
        skip_spaces(&p);
    }
    if ((mo = name_index(&p, k_months, 12)) >= 0) {
        skip_spaces(&p);
        if (!number(&p, &d)) return NAN;
    } else {
        if (!number(&p, &d)) return NAN;
        skip_spaces(&p);
        if ((mo = name_index(&p, k_months, 12)) < 0) return NAN;
    }
    skip_spaces(&p);
    neg = *p == '-';
    if (neg) p++;
    if (!number(&p, &y) || d < 1 || d > 31) return NAN;
    skip_spaces(&p);
    if (*p >= '0' && *p <= '9') {
        if (!number(&p, &h) || *p++ != ':' || !digits(&p, 2, &mi)) return NAN;
        if (*p == ':') {
            p++;
            if (!digits(&p, 2, &s)) return NAN;
        }
        if (h > 24 || mi > 59 || s > 59 || (h == 24 && (mi || s))) return NAN;
        c[3] = h;
        c[4] = mi;
        c[5] = s;
        skip_spaces(&p);
    }
    if (!strncmp(p, "GMT", 3) || !strncmp(p, "UTC", 3) || *p == 'Z') {
        p += *p == 'Z' ? 1 : 3;
        local = 0;
        if ((*p == '+' || *p == '-') && !offset(&p, &off)) return NAN;
        skip_spaces(&p);
    }
    if (*p == '(') {
        while (*p && *p != ')') p++;
        if (!*p) return NAN;
        p++;
        skip_spaces(&p);
    }
    if (*p) return NAN;
    c[0] = neg ? -y : y;
    c[1] = mo;
    c[2] = d;
    return time_clip(local ? local_to_utc(make_date(c)) : make_date(c) - off);
}

static double parse_date(PxVM *vm, PxValue s) {
    char        buf[96];
    const char *p = buf;
    if (px_str_len(s) >= sizeof buf) return NAN; /* no date string is that long */
    px_str_to_utf8(vm, s, buf, sizeof buf);
    skip_spaces(&p);
    return (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' ? parse_iso(p) : parse_legacy(p);
}

/* ------------------------------------------------------------ objects */

static PxValue date_new(PxVM *vm, double t) {
    PxDate *d = (PxDate *)px_obj_new(vm, PX_T_DATE, sizeof(PxDate), vm->protos[PX_PROTO_DATE]);
    if (!d) return PX_EXCEPTION;
    d->time = time_clip(t);
    return px_from_ptr(d);
}

static int this_time(PxVM *vm, PxValue t, double *out) {
    if (!px_is_obj(t) || px_type_of(t) != PX_T_DATE) {
        px_throw_error(vm, PX_TYPE_ERROR, "this is not a Date object");
        return -1;
    }
    *out = ((PxDate *)px_ptr(t))->time;
    return 0;
}

static PxValue fmt(PxVM *vm, double t, int which);

/* Date(): a string. new Date(), (value), (year, month[, date[, hours[,
 * minutes[, seconds[, ms]]]]]). The prototype is Date.prototype: for
 * subclasses and Reflect.construct the VM gives the result NewTarget's. */
static PxValue date_ctor(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double v;
    if (vm->native_new_target == PX_UNDEFINED) return fmt(vm, now_ms(), 0);
    if (argc == 0) {
        v = now_ms();
    } else if (argc == 1) {
        PxValue a = argv[0];
        if (px_is_obj(a) && px_type_of(a) == PX_T_DATE) {
            v = ((PxDate *)px_ptr(a))->time;
        } else {
            a = px_to_primitive(vm, a, 0);
            if (a == PX_EXCEPTION) return a;
            if (px_is_str(a)) v = parse_date(vm, a);
            else if (px_to_number(vm, a, &v) < 0) return PX_EXCEPTION;
        }
    } else {
        double c[7] = {0, 0, 1, 0, 0, 0, 0};
        int    i;
        for (i = 0; i < argc && i < 7; i++)
            if (px_to_number(vm, argv[i], &c[i]) < 0) return PX_EXCEPTION;
        c[0] = full_year(c[0]);
        v    = local_to_utc(make_date(c));
    }
    return date_new(vm, v);
}

static PxValue date_now(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    (void)t;
    (void)argc;
    (void)argv;
    return px_number(vm, now_ms());
}

static PxValue date_parse(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue s = px_to_string(vm, ARG(0));
    (void)t;
    if (s == PX_EXCEPTION) return s;
    return px_number(vm, parse_date(vm, s));
}

static PxValue date_utc(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double c[7] = {NAN, 0, 1, 0, 0, 0, 0};
    int    i;
    (void)t;
    for (i = 0; i < argc && i < 7; i++)
        if (px_to_number(vm, argv[i], &c[i]) < 0) return PX_EXCEPTION;
    c[0] = full_year(c[0]);
    return px_number(vm, time_clip(make_date(c)));
}

/* getters: magic = field * 2 + utc */
enum { F_YEAR, F_MONTH, F_DATE, F_DAY, F_HOURS, F_MINUTES, F_SECONDS, F_MS, F_TIME, F_TZ };

static PxValue datep_get(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double v;
    Fields f;
    int    field = MAGIC >> 1, utc = MAGIC & 1;
    (void)argc;
    (void)argv;
    if (this_time(vm, t, &v) < 0) return PX_EXCEPTION;
    if (field == F_TIME || isnan(v)) return px_number(vm, v);
    if (field == F_TZ) {
        double off = tz_offset(v);
        return px_number(vm, off == 0 ? 0 : -off / 60000.0); /* +0, not -0, for UTC */
    }
    split(v, !utc, &f);
    switch (field) {
    case F_YEAR: return px_int(vm, f.year);
    case F_MONTH: return px_int(vm, f.month);
    case F_DATE: return px_int(vm, f.day);
    case F_DAY: return px_int(vm, f.weekday);
    case F_HOURS: return px_int(vm, f.hour);
    case F_MINUTES: return px_int(vm, f.minute);
    case F_SECONDS: return px_int(vm, f.second);
    default: return px_int(vm, f.ms);
    }
}

/* setters: magic = field * 2 + utc; each takes its field and up to the
 * ones that follow (setHours(h, m, s, ms)) */
static PxValue datep_set(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    static const uint8_t k_first[] = {0, 1, 2, 0, 3, 4, 5, 6}, k_count[] = {3, 2, 1, 0, 4, 3, 2, 1};
    double v, a[4], c[7];
    Fields f;
    int    field = MAGIC >> 1, utc = MAGIC & 1, i, n;
    if (this_time(vm, t, &v) < 0) return PX_EXCEPTION;
    n = argc < 4 ? argc : 4;
    for (i = 0; i < n; i++)
        if (px_to_number(vm, argv[i], &a[i]) < 0) return PX_EXCEPTION;
    if (field == F_TIME) {
        ((PxDate *)px_ptr(t))->time = time_clip(n ? a[0] : NAN);
        return px_number(vm, ((PxDate *)px_ptr(t))->time);
    }
    if (isnan(v)) {
        /* only the year setters give an invalid date a value: they start
         * from +0 read as the fields of 1970-01-01T00:00 (local or UTC) */
        if (field != F_YEAR) return px_number(vm, NAN);
        split(0, 0, &f);
    } else {
        split(v, !utc, &f);
    }
    c[0] = f.year;
    c[1] = f.month;
    c[2] = f.day;
    c[3] = f.hour;
    c[4] = f.minute;
    c[5] = f.second;
    c[6] = f.ms;
    if (n == 0) a[0] = NAN, n = 1;
    for (i = 0; i < n && i < k_count[field]; i++) c[k_first[field] + i] = a[i];
    v = make_date(c);
    if (!utc) v = local_to_utc(v);
    ((PxDate *)px_ptr(t))->time = time_clip(v);
    return px_number(vm, ((PxDate *)px_ptr(t))->time);
}

/* formats: 0 toString, 1 toDateString, 2 toTimeString, 3 toISOString,
 * 4 toUTCString, 5 toLocaleString, 6 toLocaleDateString, 7 toLocaleTimeString */
static PxValue fmt(PxVM *vm, double t, int which) {
    char   buf[96], tzs[16], ys[16];
    Fields f;
    if (isnan(t)) {
        if (which == 3) return px_throw_error(vm, PX_RANGE_ERROR, "invalid time value");
        return px_str_from_cstr(vm, "Invalid Date");
    }
    split(t, which != 3 && which != 4, &f);
    {
        int off = (int)(f.tz / 60000.0), ao = off < 0 ? -off : off;
        snprintf(tzs, sizeof tzs, "GMT%c%02d%02d", off < 0 ? '-' : '+', ao / 60, ao % 60);
    }
    /* DateString's year: at least four digits, a minus sign before BC ones */
    snprintf(ys, sizeof ys, "%s%04d", f.year < 0 ? "-" : "", abs(f.year));
    switch (which) {
    case 0:
        snprintf(buf, sizeof buf, "%s %s %02d %s %02d:%02d:%02d %s", k_days[f.weekday], k_months[f.month], f.day, ys,
                 f.hour, f.minute, f.second, tzs);
        break;
    case 1: snprintf(buf, sizeof buf, "%s %s %02d %s", k_days[f.weekday], k_months[f.month], f.day, ys); break;
    case 2: snprintf(buf, sizeof buf, "%02d:%02d:%02d %s", f.hour, f.minute, f.second, tzs); break;
    case 3:
        if (f.year >= 0 && f.year <= 9999)
            snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.year, f.month + 1, f.day, f.hour,
                     f.minute, f.second, f.ms);
        else
            snprintf(buf, sizeof buf, "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.year < 0 ? '-' : '+', abs(f.year),
                     f.month + 1, f.day, f.hour, f.minute, f.second, f.ms);
        break;
    case 4:
        snprintf(buf, sizeof buf, "%s, %02d %s %s %02d:%02d:%02d GMT", k_days[f.weekday], f.day, k_months[f.month], ys,
                 f.hour, f.minute, f.second);
        break;
    case 5:
        snprintf(buf, sizeof buf, "%d/%d/%d, %d:%02d:%02d %s", f.month + 1, f.day, f.year,
                 f.hour % 12 == 0 ? 12 : f.hour % 12, f.minute, f.second, f.hour < 12 ? "AM" : "PM");
        break;
    case 6: snprintf(buf, sizeof buf, "%d/%d/%d", f.month + 1, f.day, f.year); break;
    default:
        snprintf(buf, sizeof buf, "%d:%02d:%02d %s", f.hour % 12 == 0 ? 12 : f.hour % 12, f.minute, f.second,
                 f.hour < 12 ? "AM" : "PM");
        break;
    }
    return px_str_from_cstr(vm, buf);
}

static PxValue datep_fmt(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    double v;
    (void)argc;
    (void)argv;
    if (this_time(vm, t, &v) < 0) return PX_EXCEPTION;
    return fmt(vm, v, MAGIC);
}

/* toJSON(key): generic -- any object with a toISOString method */
static PxValue datep_to_json(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue o = px_to_object(vm, t), tv, f;
    (void)argc;
    (void)argv;
    if (o == PX_EXCEPTION) return o;
    PX_ROOT(vm, o);
    tv = px_to_primitive(vm, o, 2); /* hint number */
    if (tv == PX_EXCEPTION) goto fail;
    if (px_is_num(tv) && !isfinite(px_num(tv))) {
        px_pop_roots(vm, 1);
        return PX_NULL;
    }
    f = px_get(vm, o, px_intern_cstr(vm, "toISOString"));
    if (f == PX_EXCEPTION) goto fail;
    if (!px_is_callable(f)) {
        px_throw_error(vm, PX_TYPE_ERROR, "toJSON: toISOString is not a function");
        goto fail;
    }
    tv = px_call(vm, f, o, 0, NULL);
    px_pop_roots(vm, 1);
    return tv;
fail:
    px_pop_roots(vm, 1);
    return PX_EXCEPTION;
}

/* [Symbol.toPrimitive](hint): "default" converts like "string" */
static PxValue datep_to_primitive(PxVM *vm, PxValue t, int argc, PxValue *argv) {
    PxValue hint = ARG(0), r;
    char    h[8] = "";
    int     i, string_first;
    if (!px_is_obj(t))
        return px_throw_error(vm, PX_TYPE_ERROR, "Date.prototype[Symbol.toPrimitive]: this is not an object");
    if (px_is_str(hint) && px_str_len(hint) < sizeof h) px_str_to_utf8(vm, hint, h, sizeof h);
    if (!strcmp(h, "string") || !strcmp(h, "default")) string_first = 1;
    else if (!strcmp(h, "number")) string_first = 0;
    else return px_throw_error(vm, PX_TYPE_ERROR, "invalid hint for Symbol.toPrimitive");
    /* OrdinaryToPrimitive */
    for (i = 0; i < 2; i++) {
        PxValue f = px_get(vm, t, vm->atom[(i == 0) == string_first ? PX_ATOM_toString : PX_ATOM_valueOf]);
        if (f == PX_EXCEPTION) return f;
        if (!px_is_callable(f)) continue;
        r = px_call(vm, f, t, 0, NULL);
        if (r == PX_EXCEPTION || !px_is_obj(r)) return r;
    }
    return px_throw_error(vm, PX_TYPE_ERROR, "cannot convert the Date to a primitive value");
}

int px_date_init(PxVM *vm) {
    static const PxFnDef statics[] = {{"now", date_now, 0, 0}, {"parse", date_parse, 1, 0}, {"UTC", date_utc, 7, 0}};
    static const PxFnDef methods[] = {
        {"getFullYear", datep_get, 0, F_YEAR * 2},       {"getUTCFullYear", datep_get, 0, F_YEAR * 2 + 1},
        {"getMonth", datep_get, 0, F_MONTH * 2},         {"getUTCMonth", datep_get, 0, F_MONTH * 2 + 1},
        {"getDate", datep_get, 0, F_DATE * 2},           {"getUTCDate", datep_get, 0, F_DATE * 2 + 1},
        {"getDay", datep_get, 0, F_DAY * 2},             {"getUTCDay", datep_get, 0, F_DAY * 2 + 1},
        {"getHours", datep_get, 0, F_HOURS * 2},         {"getUTCHours", datep_get, 0, F_HOURS * 2 + 1},
        {"getMinutes", datep_get, 0, F_MINUTES * 2},     {"getUTCMinutes", datep_get, 0, F_MINUTES * 2 + 1},
        {"getSeconds", datep_get, 0, F_SECONDS * 2},     {"getUTCSeconds", datep_get, 0, F_SECONDS * 2 + 1},
        {"getMilliseconds", datep_get, 0, F_MS * 2},     {"getUTCMilliseconds", datep_get, 0, F_MS * 2 + 1},
        {"getTime", datep_get, 0, F_TIME * 2},           {"valueOf", datep_get, 0, F_TIME * 2},
        {"getTimezoneOffset", datep_get, 0, F_TZ * 2},
        {"setFullYear", datep_set, 3, F_YEAR * 2},       {"setUTCFullYear", datep_set, 3, F_YEAR * 2 + 1},
        {"setMonth", datep_set, 2, F_MONTH * 2},         {"setUTCMonth", datep_set, 2, F_MONTH * 2 + 1},
        {"setDate", datep_set, 1, F_DATE * 2},           {"setUTCDate", datep_set, 1, F_DATE * 2 + 1},
        {"setHours", datep_set, 4, F_HOURS * 2},         {"setUTCHours", datep_set, 4, F_HOURS * 2 + 1},
        {"setMinutes", datep_set, 3, F_MINUTES * 2},     {"setUTCMinutes", datep_set, 3, F_MINUTES * 2 + 1},
        {"setSeconds", datep_set, 2, F_SECONDS * 2},     {"setUTCSeconds", datep_set, 2, F_SECONDS * 2 + 1},
        {"setMilliseconds", datep_set, 1, F_MS * 2},     {"setUTCMilliseconds", datep_set, 1, F_MS * 2 + 1},
        {"setTime", datep_set, 1, F_TIME * 2},
        {"toString", datep_fmt, 0, 0},                   {"toDateString", datep_fmt, 0, 1},
        {"toTimeString", datep_fmt, 0, 2},               {"toISOString", datep_fmt, 0, 3},
        {"toUTCString", datep_fmt, 0, 4},                {"toLocaleString", datep_fmt, 0, 5},
        {"toLocaleDateString", datep_fmt, 0, 6},         {"toLocaleTimeString", datep_fmt, 0, 7},
        {"toJSON", datep_to_json, 1, 0},
    };
    PxValue proto = vm->protos[PX_PROTO_DATE], ctor = px_make_native(vm, date_ctor, "Date", 7, 0), f;
    if (ctor == PX_EXCEPTION) return -1;
    vm->ctors[PX_PROTO_DATE] = ctor;
    if (px_def_value(vm, vm->global, "Date", ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_value(vm, ctor, "prototype", proto, 0) < 0 ||
        px_define(vm, proto, vm->atom[PX_ATOM_constructor], ctor, PX_ATTR_HIDDEN) < 0 ||
        px_def_fns(vm, ctor, statics, PX_COUNTOF(statics)) < 0 ||
        px_def_fns(vm, proto, methods, PX_COUNTOF(methods)) < 0)
        return -1;
    /* Annex B: toGMTString is the same function as toUTCString */
    f = px_get(vm, proto, px_intern_cstr(vm, "toUTCString"));
    if (f == PX_EXCEPTION || px_def_value(vm, proto, "toGMTString", f, PX_ATTR_HIDDEN) < 0) return -1;
    f = px_make_native(vm, datep_to_primitive, "[Symbol.toPrimitive]", 1, 0);
    if (f == PX_EXCEPTION) return -1;
    ((PxObject *)px_ptr(f))->flags |= PX_OBJ_NOT_CTOR;
    return px_define(vm, proto, vm->sym_to_primitive, f, PX_ATTR_CONFIGURABLE);
}
