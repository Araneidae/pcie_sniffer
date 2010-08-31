/* Parsing support. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"
#include "parse.h"


/* Checks whether a string has been fully parsed.  If no end pointer is
 * specified, then the string must be consumed or an error is generated, but if
 * an end pointer is given then the string is merely assigned to it. */
bool parse_end(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character");
}


bool parse_char(const char **string, char ch)
{
    return **string == ch  &&  DO_(*string += 1);
}


bool parse_int(const char **string, int *result)
{
    const char *start = *string;
    *result = strtol(start, (char **) string, 0);
    return TEST_OK_(*string > start, "Number missing");
}


bool parse_uint(const char **string, unsigned int *result)
{
    const char *start = *string;
    *result = strtoul(start, (char **) string, 0);
    return TEST_OK_(*string > start, "Number missing");
}


bool parse_uint32(const char **string, uint32_t *result)
{
    const char *start = *string;
    *result = strtoul(start, (char **) string, 0);
    return TEST_OK_(*string > start, "Number missing");
}


bool parse_uint64(const char **string, uint64_t *result)
{
    const char *start = *string;
    *result = strtoull(start, (char **) string, 0);
    return TEST_OK_(*string > start, "Number missing");
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


bool report_parse_error(
    const char *message, bool ok, const char *string, const char **end)
{
    if (ok)
        ok = parse_end(end);
    char error_message[1024];
    if (!ok)
    {
        /* Convert local parse error message into a more global message. */
        snprintf(error_message, sizeof(error_message),
            "Error parsing %s: %s at offset %d in \"%s\"",
            message, get_error_message(), *end - string, string);
    }
    pop_error_handling();
    return TEST_OK_(ok, "%s", error_message);
}
