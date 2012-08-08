/* Testing how useful the backtrace library is.  Answer, not very. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <fenv.h>

double zero = 0.0;

static void print_backtrace(void)
{
    void *buffer[1024];
    int size = backtrace(buffer, 1024);
    char **strings = backtrace_symbols(buffer, size);
    printf("%d\n", size);
    for (int i = 0; i < size; i++)
        printf("%s\n", strings[i]);
    free(strings);
}

static void dummy(void)
{
    print_backtrace();
    printf("About to divide by zero\n");
    printf("%g\n", 1.0 / zero);
}

static void sig_handler(int sig)
{
    printf("signal %d\n", sig);
    print_backtrace();
    exit(1);
}

int main(int argc, char **argv)
{
    signal(SIGFPE, sig_handler);
    feenableexcept(FE_DIVBYZERO | FE_INVALID);
    dummy();
    return 0;
}
