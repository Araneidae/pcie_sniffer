/* Helper macros and declarations to simplify error handling. */

#define unlikely(x)   __builtin_expect((x), 0)

void print_error(const char * Message, ...)
    __attribute__((format(printf, 1, 2)));

void panic_error(const char * filename, int line)
    __attribute__((__noreturn__));


/* Generic TEST macro: computes a boolean from expr using COND (should be a
 * macro), and prints the given error message if the boolean is false.  The
 * boolean result is the value of the entire expression. */
#define TEST_(COND, expr, message...) \
    ( { \
        bool __ok__ = COND(expr); \
        if (unlikely(!__ok__)) \
            print_error(message); \
        __ok__; \
    } )

/* Default error message for unexpected errors. */
#define ERROR_MESSAGE   \
    "Unexpected error at %s:%d", __FILE__, __LINE__

/* An assert for tests that really really should not fail!  These exit
 * immediately. */
#define ASSERT_(COND, expr)  \
    do { \
        if (unlikely(!COND(expr))) \
            panic_error(__FILE__, __LINE__); \
    } while (0)


/* Tests system calls: -1 => error. */
#define _COND_IO(expr)       ((int) (expr) != -1)
#define TEST_IO_(expr, message...)      TEST_(_COND_IO, expr, message)
#define TEST_IO(expr)                   TEST_IO_(expr, ERROR_MESSAGE)
#define ASSERT_IO(expr)                 ASSERT_(_COND_IO, expr)

/* Tests pointers: NULL => error. */
#define _COND_NULL(expr)     ((expr) != NULL)
#define TEST_NULL_(expr, message...)    TEST_(_COND_NULL, expr, message)
#define TEST_NULL(expr)                 TEST_NULL_(expr, ERROR_MESSAGE)
#define ASSERT_NULL(expr)               ASSERT_(_COND_NULL, expr)

/* Tests an ordinary boolean: false => error. */
#define _COND_OK(expr)       ((bool) (expr))
#define TEST_OK_(expr, message...)      TEST_(_COND_OK, expr, message)
#define TEST_OK(expr)                   TEST_OK_(expr, ERROR_MESSAGE)
#define ASSERT_OK(expr)                 ASSERT_(_COND_OK, expr)

/* Tests the return from a pthread_ call: a non zero return is the error
 * code!  We just assign this to errno. */
#define _COND_0(expr) \
    ( { \
        int __rc__ = (expr); \
        if (unlikely(__rc__ != 0)) \
            errno = __rc__; \
        __rc__ == 0; \
    } )
#define TEST_0_(expr, message...)       TEST_(_COND_0, expr, message)
#define TEST_0(expr)                    TEST_0_(expr, ERROR_MESSAGE)
#define ASSERT_0(expr)                  ASSERT_(_COND_0, expr)



/* These two macros facilitate using the macros above by creating if
 * expressions that're slightly more sensible looking than ?: in context. */
#define DO_(action)                     ({action; true;})
#define IF_(test, iftrue)               ((test) ? (iftrue) : true)
#define IF_ELSE(test, iftrue, iffalse)  ((test) ? (iftrue) : (iffalse))
