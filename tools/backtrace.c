#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <fenv.h>

double zero = 0.0;

void print_backtrace(void)
{
    void *buffer[1024];
    int size = backtrace(buffer, 1024);
    char **strings = backtrace_symbols(buffer, size);
    printf("%d\n");
    for (int i = 0; i < size; i++)
        printf("%s\n", strings[i]);
    free(strings);
}

void dummy(void)
{
    print_backtrace();
    printf("About to divide by zero\n");
    printf("%g\n", 1.0 / zero);
}

void sig_handler(int signal)
{
    printf("signal %d\n", signal);
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
