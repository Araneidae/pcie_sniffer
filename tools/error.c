#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>

#include "error.h"
#include "locking.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Local error handling. */

struct error_stack {
    char *message;
    struct error_stack *last;
};

static __thread struct error_stack *error_stack = NULL;

void push_error_handling(void)
{
    struct error_stack *new_entry = malloc(sizeof(struct error_stack));
    new_entry->message = NULL;
    new_entry->last = error_stack;
    error_stack = new_entry;
}

void pop_error_handling(char **error_message)
{
    struct error_stack *top = error_stack;
    error_stack = top->last;
    if (error_message)
        *error_message = top->message;
    else
        free(top->message);
    free(top);
}

const char * get_error_message(void)
{
    return error_stack->message;
}

void reset_error_message(void)
{
    struct error_stack *top = error_stack;
    free(top->message);
    top->message = NULL;
}


/* Takes ownership of message if the error stack is non-empty. */
static bool save_message(char *message)
{
    struct error_stack *top = error_stack;
    if (top)
    {
        free(top->message);
        top->message = message;
        return true;
    }
    else
        return false;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Printf to the heap. */

char * vhprintf(const char *format, va_list args)
{
    /* To determine how long the resulting string should be we print the string
     * twice, first into an empty buffer. */
    int length = vsnprintf(NULL, 0, format, args) + 1;
    char *buffer = malloc(length);
    vsnprintf(buffer, length, format, args);
    return buffer;
}


char * hprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    return vhprintf(format, args);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Error handling and logging. */

DECLARE_LOCKING(lock);


/* Determines whether error messages go to stderr or syslog. */
static bool daemon_mode = false;
/* Determines whether to log non-error messages. */
static bool verbose = false;


void verbose_logging(bool verbose_)
{
    verbose = verbose_;
}

void start_logging(const char *ident)
{
    openlog(ident, 0, LOG_DAEMON);
    daemon_mode = true;
}


void vlog_message(int priority, const char *format, va_list args)
{
    LOCK(lock);
    if (daemon_mode)
        vsyslog(priority, format, args);
    else
    {
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
    }
    UNLOCK(lock);
}

void log_message(const char * message, ...)
{
    if (verbose)
    {
        va_list args;
        va_start(args, message);
        vlog_message(LOG_INFO, message, args);
    }
}

void log_error(const char * message, ...)
{
    va_list args;
    va_start(args, message);
    vlog_message(LOG_ERR, message, args);
}


static char * add_strerror(char *message, int last_errno)
{
    if (last_errno == 0)
        return message;
    else
    {
        /* This is very annoying: strerror() is not not necessarily thread
         * safe ... but not for any compelling reason, see:
         *  http://sources.redhat.com/ml/glibc-bugs/2005-11/msg00101.html
         * and the rather unhelpful reply:
         *  http://sources.redhat.com/ml/glibc-bugs/2005-11/msg00108.html
         *
         * On the other hand, the recommended routine strerror_r() is
         * inconsistently defined -- depending on the precise library and its
         * configuration, it returns either an int or a char*.  Oh dear.
         *
         * Ah well.  We go with the GNU definition, so here is a buffer to
         * maybe use for the message. */
        char StrError[64];
        char *result = hprintf("%s: (%d) %s", message, last_errno,
            strerror_r(last_errno, StrError, sizeof(StrError)));
        free(message);
        return result;
    }
}


void print_error(const char * format, ...)
{
    int last_errno = errno;
    va_list args;
    va_start(args, format);
    char *message = add_strerror(vhprintf(format, args), last_errno);
    if (!save_message(message))
    {
        log_error("%s", message);
        free(message);
    }
}


void panic_error(const char * filename, int line)
{
    int last_errno = errno;
    char *message = add_strerror(
        hprintf("panic at %s, line %d", filename, line), last_errno);
    log_error("%s", message);
    free(message);

    fflush(stderr);
    _exit(255);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Utility function with no proper home. */


void dump_binary(FILE *out, const void *buffer, size_t length)
{
    const uint8_t *dump = buffer;

    for (size_t a = 0; a < length; a += 16)
    {
        fprintf(out, "%08zx: ", a);
        for (int i = 0; i < 16; i ++)
        {
            if (a + i < length)
                fprintf(out, " %02x", dump[a+i]);
            else
                fprintf(out, "   ");
            if (i % 16 == 7)
                fprintf(out, " ");
        }

        fprintf(out, "  ");
        for (int i = 0; i < 16; i ++)
        {
            uint8_t c = dump[a+i];
            if (a + i < length)
                fprintf(out, "%c", 32 <= c  &&  c < 127 ? c : '.');
            else
                fprintf(out, " ");
            if (i % 16 == 7)
                fprintf(out, " ");
        }
        fprintf(out, "\n");
    }
    if (length % 16 != 0)
        fprintf(out, "\n");
}

