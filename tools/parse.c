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


/* Checks whether a string has been fully parsed.  If no end pointer is
 * specified, then the string must be consumed or an error is generated, but if
 * an end pointer is given then the string is merely assigned to it. */
bool parse_end(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character");
}


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


bool parse_datetime(const char **string, struct timespec *ts)
{
    errno = 0;
    struct tm tm;
    int nsecs = 0;
    char *end = strptime(*string, "%Y-%m-%dT%H:%M:%S", &tm);
    bool ok =
        TEST_NULL_(end, "Incomplete date time")  &&
        check_number(*string, end);
    if (ok)
    {
        *string = end;
        if (read_char(string, '.')  &&  '0' <= **string  &&  **string <= '9')
        {
            /* Annoyingly complicated.  Just want to interpret .nnnnnnnn as an
             * integer nanoseconds, but want it to behave as if it was a decimal
             * fraction -- so need to count number of digits parsed and fixup
             * afterwards! */
            nsecs = strtoul(*string, &end, 10);
            int digits = end - *string;
            *string = end;
            ok = TEST_OK_(digits <= 9, "Too many digits for ns");
            for ( ; ok  &&  digits < 9; digits ++)
                nsecs *= 10;
        }
    }
    if (ok)
    {
        /* Convert (tm,nsecs) value into timespec value for return.  Note that
         * the more standard function to use here is mktime(), but that depends
         * on the value of the TZ environment variable. */
        ts->tv_sec = timegm(&tm);
        ts->tv_nsec = nsecs;
        ok = TEST_IO_(ts->tv_sec, "Invalid date");
    }
    return ok;
}


bool report_parse_error(
    const char *message, bool ok, const char *string, const char **end)
{
    if (ok  &&  parse_end(end))
    {
        pop_error_handling();
        return true;
    }
    else
    {
        /* Convert local parse error message into a more global message.  We
         * have to hang onto the new error message while we pop the error
         * context so the new message becomes the error message. */
        char *error_message = hprintf(
            "Error parsing %s: %s at offset %d in \"%s\"",
            message, get_error_message(), *end - string, string);
        pop_error_handling();
        print_error("%s", error_message);
        free(error_message);
        return false;
    }
}
