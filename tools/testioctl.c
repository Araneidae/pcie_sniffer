#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "error.h"
#include "../fa_sniffer.h"

#define K 1024
#define M (K * K)


bool running = true;

void handler(int sig)
{
    printf("signal %d\n", sig);
    running = false;
}


void print_error(const char *message, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, message);
    vsprintf(buffer, message, args);
    perror(buffer);
    errno = 0;
}

void status(int f)
{
    int i;
    struct fa_status status;
    TEST_IO(ioctl(f, FASNIF_IOCTL_GET_STATUS, &status));
    printf("status: %x, %x, %x, %s, %s, %u\n",
        status.status, status.partner, status.last_interrupt,
        status.running ? "running" : "stopped",
        status.overrun ? "overrun" : "ok", status.available);
}

void restart(f)
{
    TEST_IO(ioctl(f, FASNIF_IOCTL_RESTART));
}

void do_read(int f, size_t amount)
{
    char buffer[65536];
    size_t residue = amount;
    bool ok = true;
    while (running  &&  ok  &&  residue > 0)
    {
        size_t target = residue > sizeof(buffer) ? sizeof(buffer) : residue;
        ok = TEST_read_(f, buffer, target, "Underrun");
        residue -= target;
    }
    printf("do_read %u => %u\n", amount, residue);
}

void do_sleep(unsigned int time)
{
    printf("sleeping %u\n", time);
    TEST_IO(sleep(time));
}

void set_signal(void)
{
    struct sigaction sa = { .sa_handler = handler, .sa_flags = 0 };
    TEST_IO(sigfillset(&sa.sa_mask))  &&
    TEST_IO(sigaction(SIGINT, &sa, NULL));
//     TEST_IO(signal(SIGINT, handler));
}

int main(int argc, char **argv)
{
    int f;
    TEST_IO(f = open("/dev/fa_sniffer0", O_RDONLY));
    status(f);
    do_read(f, 1*M);
    status(f);
    do_sleep(1);
    status(f);
    do_read(f, 3*M);
    status(f);
    restart(f);
    status(f);
    do_read(f, 3*M);
    status(f);
    set_signal();
    while (running)
        do_read(f, 40*M);
}
