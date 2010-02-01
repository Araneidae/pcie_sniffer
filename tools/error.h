/* Helper macros and declarations to simplify error handling. */

#define unlikely(x)   __builtin_expect((x), 0)

void print_error(const char * Message, ...)
    __attribute__((format(printf, 1, 2)));

void panic_error(const char * filename, int line);


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


/* Tests system calls: -1 => error. */
#define _COND_IO(expr)       ((int) (expr) != -1)
#define TEST_IO(expr, message...)       TEST_(_COND_IO,   expr, message)

/* Tests pointers: NULL => error. */
#define _COND_NULL(expr)     ((expr) != NULL)
#define TEST_NULL(expr, message...)     TEST_(_COND_NULL, expr, message)

/* Tests an ordinary boolean: false => error. */
#define _COND_OK(expr)       ((bool) (expr))
#define TEST_OK(expr, message...)       TEST_(_COND_OK,   expr, message)

/* Tests the return from a pthread_ call: a non zero return is the error
 * code!  We just assign this to errno. */
#define _COND_0(expr) \
    ( { \
        int __rc__ = (expr); \
        if (unlikely(__rc__ != 0)) \
            errno = __rc__; \
        __rc__ == 0; \
    } )
#define TEST_0(expr, message...)        TEST_(_COND_0,    expr, message)


/* These two macros facilitate using the macros above by creating if
 * expressions that're slightly more sensible looking than ?: in context. */
#define DO_(action)                     ({action; true;})
#define IF_(test, iftrue)               ((test) ? (iftrue) : true)
#define IF_ELSE(test, iftrue, iffalse)  ((test) ? (iftrue) : (iffalse))



/* An assert for tests that really really should not fail! */
#define ASSERT_(test, expr)  \
    do { \
        if (!TEST_##test(expr, "assert failed")) \
            panic_error(__FILE__, __LINE__); \
    } while (0)

#define ASSERT_IO(expr)     ASSERT_(IO, expr)
#define ASSERT_NULL(expr)   ASSERT_(NULL, expr)
#define ASSERT_OK(expr)     ASSERT_(OK, expr)
#define ASSERT_0(expr)      ASSERT_(0, expr)
