/* Common parsing support. */

/* All of these parse routines take two arguments and return a boolean value:
 *
 *  bool parse_<object>(const char **string, typeof(<object>) *result);
 *
 * After a successful parse *string points after the parsed part of the string,
 * otherwise it points to the point where an error was detected. */

bool parse_int(const char **string, int *result);
bool parse_uint(const char **string, unsigned int *result);
bool parse_uint32(const char **string, uint32_t *result);
bool parse_uint64(const char **string, uint64_t *result);
bool parse_double(const char **string, double *result);

/* Integer possibly followed by K or M. */
bool parse_size32(const char **string, uint32_t *result);
/* Integer possibly followed by K, M, G or T. */
bool parse_size64(const char **string, uint64_t *result);

/* Parses date and time in ISO format with an optional trailing nanoseconds
 * part, ie:
 *      yyyy-mm-ddThh:mm:ss[.nnnnnnnnnn] . */
struct timespec;
bool parse_datetime(const char **string, struct timespec *ts);
/* Parses time of day in ISO 8601 format optional nanoseconds:
 *      hh:mm:ss[.nnnnnnnnnn] . */
bool parse_time(const char **string, struct timespec *ts);
/* Parses timestamp in format: secs[.nnn] */
bool parse_seconds(const char **string, struct timespec *ts);

/* Only succeeds if **string=='\0'. */
bool parse_end(const char **string);

/* Checks for presence of ch, consumes it if present.  No error is generated if
 * ch is not found, unlike the parse functions. */
bool read_char(const char **string, char ch);
/* Like read_char(), but generates an error if ch is not found. */
bool parse_char(const char **string, char ch);

/* Wraps parsing of a complete string and generation of a suitable error
 * message. */
#define DO_PARSE(message, parse, string, result...) \
    ( { \
      const char *__string__ = (string); \
      push_error_handling(); \
      report_parse_error((message), \
          (parse)(&__string__, ##result), (string), &__string__); \
    } )

/* This must be called with push_error_handling() in force, and will call
 * pop_error_handling() before returning.  Designed to be wrapped by the
 * DO_PARSE macro above. */
bool report_parse_error(
    const char *message, bool ok, const char *string, const char **end);
