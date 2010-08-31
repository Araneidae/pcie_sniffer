#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <syslog.h>

#include "error.h"


/* Fixed on-stack buffers for message logging. */
#define MESSAGE_LENGTH  512


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

void pop_error_handling(void)
{
    struct error_stack *top = error_stack;
    error_stack = top->last;
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


static bool save_message(const char *message)
{
    struct error_stack *top = error_stack;
    if (top)
    {
        top->message = realloc(top->message, strlen(message) + 1);
        strcpy(top->message, message);
        return true;
    }
    else
        return false;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Error handling and logging. */

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
    char message[MESSAGE_LENGTH];
    vsnprintf(message, sizeof(message), format, args);
    if (daemon_mode)
        syslog(priority, "%s", message);
    else
        fprintf(stderr, "%s\n", message);
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



void print_error(const char * message, ...)
{
    /* Large enough not to really worry about overflow.  If we do generate a
     * silly message that's too big, then that's just too bad. */
    int error = errno;
    char error_message[MESSAGE_LENGTH];
    va_list args;
    va_start(args, message);

    int Count = vsnprintf(error_message, MESSAGE_LENGTH, message, args);
    if (error != 0)
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
        char StrError[MESSAGE_LENGTH];
        snprintf(error_message + Count, MESSAGE_LENGTH - Count,
            ": (%d) %s", error, strerror_r(error, StrError, sizeof(StrError)));
    }
    if (!save_message(error_message))
        log_error("%s", error_message);
}


void panic_error(const char * filename, int line)
{
    print_error("panic at %s, line %d", filename, line);
    fflush(stderr);
    _exit(255);
}
