/* Parsing support. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#include "error.h"
#include "parse.h"


/* Checks whether a string has been fully parsed. */
bool parse_end(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character");
}


/* Called after a C library conversion function checks that anything was
 * converted and that the conversion was successful.  Relies on errno being zero
 * before conversion started. */
static bool check_number(const char *start, const char *end)
{
    return
        TEST_OK_(end > start, "Number missing")  &&
        TEST_OK_(errno == 0, "Error converting number");
}


/* Parsing numbers is rather boilerplate.  This macro encapsulates everything in
 * one common form. */
#define DEFINE_PARSE_NUM(name, type, convert, extra...) \
    bool name(const char **string, type *result) \
    { \
        errno = 0; \
        const char *start = *string; \
        *result = convert(start, (char **) string, ##extra); \
        return check_number(start, *string); \
    }

DEFINE_PARSE_NUM(parse_int,    int,          strtol, 0)
DEFINE_PARSE_NUM(parse_uint,   unsigned int, strtoul, 0)
DEFINE_PARSE_NUM(parse_uint32, uint32_t,     strtoul, 0)
DEFINE_PARSE_NUM(parse_uint64, uint64_t,     strtoull, 0)
DEFINE_PARSE_NUM(parse_double, double,       strtod)



bool read_char(const char **string, char ch)
{
    return **string == ch  &&  DO_(*string += 1);
}


bool parse_char(const char **string, char ch)
{
    return TEST_OK_(read_char(string, ch), "Character '%c' expected", ch);
}


bool parse_size32(const char **string, uint32_t *result)
{
    bool ok = parse_uint32(string, result);
    if (ok)
        switch (**string)
        {
            case 'K':   *result <<= 10;  (*string) ++;  break;
            case 'M':   *result <<= 20;  (*string) ++;  break;
        }
    return ok;
}


bool parse_size64(const char **string, uint64_t *result)
{
    bool ok = parse_uint64(string, result);
    if (ok)
        switch (**string)
        {
            case 'K':   *result <<= 10;  (*string) ++;  break;
            case 'M':   *result <<= 20;  (*string) ++;  break;
            case 'G':   *result <<= 30;  (*string) ++;  break;
            case 'T':   *result <<= 40;  (*string) ++;  break;
        }
    return ok;
}


/* Parses optional number of the form .nnnnnnnnn expressing a fraction of a
 * second, converts into nanoseconds, allowing up to 9 digits. */
static bool parse_nanoseconds(const char **string, long *nsec)
{
    bool ok = true;
    if (read_char(string, '.')  &&  '0' <= **string  &&  **string <= '9')
    {
        /* Annoyingly complicated.  Just want to interpret .nnnnnnnn as an
         * integer nanoseconds, but want it to behave as if it was a decimal
         * fraction -- so need to count number of digits parsed and fixup
         * afterwards! */
        char *end;
        *nsec = strtoul(*string, &end, 10);
        int digits = end - *string;
        *string = end;
        ok = TEST_OK_(digits <= 9, "Too many digits for ns");
        for ( ; ok  &&  digits < 9; digits ++)
            *nsec *= 10;
    }
    else
        *nsec = 0;
    return ok;
}


static bool parse_date_or_time(
    const char *format, const char *error_message,
    const char **string, struct tm *tm, long *nsecs)
{
    char *end;  // Mustn't assign NULL to *string!
    return
        TEST_NULL_(
            end = strptime(*string, format, tm), "%s", error_message)  &&
        DO_(*string = end)  &&
        parse_nanoseconds(string, nsecs);
}


bool parse_time(const char **string, struct timespec *ts)
{
    struct tm tm;
    return
        parse_date_or_time(
            "%H:%M:%S", "Incomplete time, should be hh:mm:ss",
            string, &tm, &ts->tv_nsec)  &&
        DO_(ts->tv_sec = tm.tm_sec + 60 * (tm.tm_min + 60 * tm.tm_hour));
}


bool parse_datetime(const char **string, struct timespec *ts)
{
    struct tm tm;
    return
        parse_date_or_time(
            "%Y-%m-%dT%H:%M:%S",
            "Incomplete date time, should be yyyy-mm-ddThh:mm:ss",
            string, &tm, &ts->tv_nsec)  &&
        /* Convert tm value into seconds for return.  Note that the more
         * standard function to use here is mktime(), but that depends on the
         * value of the TZ environment variable. */
        TEST_IO_(ts->tv_sec = timegm(&tm), "Invalid date");
}


bool parse_seconds(const char **string, struct timespec *ts)
{
    int sec;
    return
        parse_int(string, &sec)  &&
        parse_nanoseconds(string, &ts->tv_nsec)  &&
        DO_(ts->tv_sec = sec);
}


bool report_parse_error(
    const char *message, bool ok, const char *string, const char **end)
{
    if (ok  &&  parse_end(end))
    {
        pop_error_handling(NULL);
        return true;
    }
    else
    {
        char *error_message;
        pop_error_handling(&error_message);
        print_error(
            "Error parsing %s: %s at offset %zd in \"%s\"",
            message, error_message, *end - string, string);
        free(error_message);
        return false;
    }
}
