/* Simple server for archive data.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "archiver.h"
#include "mask.h"
#include "buffer.h"
#include "reader.h"
#include "parse.h"

#include "socket_server.h"


static void __attribute__((format(printf, 2, 3)))
    write_string(int sock, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    char *buffer = alloca(len + 1);
    vsnprintf(buffer, len + 1, format, args);
    if (!TEST_write(sock, buffer, len))
        log_error("Unable to write \"%s\" to socket %d, error: %s\n",
            buffer, sock, get_error_message());
}


/* We provide a very limited set of commands:
 *  CQ      Closes archive server
 *  CS      Prints a simple status report
 *  CF      Returns current sample frequency
 */
static bool process_command(int scon, const char *buf)
{
    switch (buf[1])
    {
        case 'Q':
            log_message("Shutdown command received");
            shutdown_archiver();
            return true;
        case 'F':
            write_string(scon, "%f\n", get_mean_frame_rate());
            return true;
        case 'S':
            return TEST_OK_(false, "Status not implemented");
        default:
            return TEST_OK_(false, "Unknown command");
    }
}


/* A subscribe request is either S<mask> or SR<raw-mask>. */
static bool parse_subscription(const char **string, filter_mask_t mask)
{
    parse_char(string, 'S');
    if (parse_char(string, 'R'))
        return parse_raw_mask(string, mask);
    else
        return parse_mask(string, mask);
}


/* A subscription is a command of the form S<mask> where <mask> is a mask
 * specification as described in mask.h.  The default mask is empty. */
static bool process_subscribe(int scon, const char *buf)
{
    filter_mask_t mask;
    bool parse_ok = DO_PARSE("subscription", parse_subscription, buf, mask);
    if (parse_ok)
    {
        struct reader_state *reader = open_reader(false);
        bool ok = true;
        while (ok)
        {
            const void *block = get_read_block(reader, NULL, NULL);
            ok = TEST_OK(block != NULL);
            if (ok)
            {
                ok = write_frames(
                    scon, mask, block, fa_block_size / FA_FRAME_SIZE);
                ok = TEST_OK_(
                    release_read_block(reader),
                    "Write underrun to client")  &&  ok;
            }
        }
        close_reader(reader);
    }
    return parse_ok;
}


static bool process_error(int scon, const char *buf)
{
    return TEST_OK_(false, "Invalid command");
}


struct command_table {
    char id;            // Identification character
    bool (*process)(int scon, const char *buf);
} command_table[] = {
    { 'C', process_command },
    { 'R', process_read },
    { 'S', process_subscribe },
    { 0,   process_error }
};


/* Reads from the given socket until one of the following is encountered: a
 * newline (the preferred case), end of input, end of buffer or an error.  The
 * newline and anything following is discarded. */
static bool read_line(int sock, char *buf, size_t buflen)
{
    ssize_t rx;
    while (
        TEST_OK_(buflen > 0, "Read buffer exhausted")  &&
        TEST_IO(rx = read(sock, buf, buflen))  &&
        TEST_OK_(rx > 0, "End of file on input"))
    {
        char *newline = memchr(buf, '\n', rx);
        if (newline)
        {
            *newline = '\0';
            return true;
        }

        buflen -= rx;
        buf += rx;
    }
    return false;
}


static void * process_connection(void *context)
{
    int scon = (intptr_t) context;

    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (TEST_IO(getpeername(scon, (struct sockaddr *) &name, &namelen)))
    {
        uint8_t * ip = (uint8_t *) &name.sin_addr.s_addr;
        log_message("Connected from %u.%u.%u.%u:%u",
            ip[0], ip[1], ip[2], ip[3], ntohs(name.sin_port));
    }


    push_error_handling();
    char buf[4096];
    if (read_line(scon, buf, sizeof(buf)))
    {
        log_message("Read: \"%s\"", buf);
        /* Command successfully read, dispatch it to the appropriate handler. */
        struct command_table * command = command_table;
        while (command->id  &&  command->id != buf[0])
            command += 1;

        if (!command->process(scon, buf))
            write_string(scon, "%s\n", get_error_message());
    }
    pop_error_handling();

    TEST_IO(close(scon));
    return NULL;
}


static void * run_server(void *context)
{
    int sock = (int)(intptr_t) context;
    /* Note that we need to create the spawned threads with DETACHED attribute,
     * otherwise we accumlate internal joinable state information and eventually
     * run out of resources. */
    pthread_attr_t attr;
    ASSERT_0(pthread_attr_init(&attr));
    ASSERT_0(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

    int scon;
    pthread_t thread;
    while (TEST_IO(scon = accept(sock, NULL, NULL)))
        TEST_0(pthread_create(&thread, &attr,
            process_connection, (void *)(intptr_t) scon));
    return NULL;
}


static pthread_t server_thread;

bool initialise_server(int port)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    int sock;
    return
        TEST_IO(sock = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO(bind(sock, (struct sockaddr *) &sin, sizeof(sin)))  &&
        TEST_IO(listen(sock, 5))  &&
        TEST_0(pthread_create(
            &server_thread, NULL, run_server, (void *)(intptr_t) sock))  &&
        DO_(log_message("Server listening on port %d", port));
}


void terminate_server(void)
{
    TEST_0(pthread_cancel(server_thread));
    /* We probably need to kill all the client threads. */
}
