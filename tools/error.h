#define unlikely(x)   __builtin_expect((x), 0)

void print_error(const char * Message, ...)
    __attribute__((format(printf, 1, 2)));

void panic_error(const char * filename, int line);

#define TEST_(COND, expr, message...) \
    ( { \
        bool __ok__ = COND(expr); \
        if (unlikely(!__ok__)) \
            print_error(message); \
        __ok__; \
    } )

#define COND_IO(expr)       ((int) (expr) != -1)
#define COND_NULL(expr)     ((expr) != NULL)
#define COND_OK(expr)       ((bool) (expr))
#define COND_0(expr) \
    ( { \
        int __rc__ = (expr); \
        if (unlikely(__rc__ != 0)) \
            errno = __rc__; \
        __rc__ == 0; \
    } )

#define TEST_IO(expr, message...)       TEST_(COND_IO,   expr, message)
#define TEST_NULL(expr, message...)     TEST_(COND_NULL, expr, message)
#define TEST_OK(expr, message...)       TEST_(COND_OK,   expr, message)
#define TEST_0(expr, message...)        TEST_(COND_0,    expr, message)


/* These two macros facilitate using the macros above by creating if
 * expressions that're slightly more sensible looking than ?: in context. */
#define DO_(action)                     ({action; true;})
#define IF_(test, iftrue)               ((test) ? (iftrue) : true)
#define IF_ELSE(test, iftrue, iffalse)  ((test) ? (iftrue) : (iffalse))

/* An assert for tests that really really should not fail! */
#define ASSERT(test, expr)  \
    do { \
        if (!TEST_##test(expr, "assert failed")) \
            panic_error(__FILE__, __LINE__); \
    } while (0)

#define ASSERT_IO(expr)     ASSERT(IO, expr)
#define ASSERT_NULL(expr)   ASSERT(NULL, expr)
#define ASSERT_OK(expr)     ASSERT(OK, expr)
#define ASSERT_0(expr)      ASSERT(0, expr)
