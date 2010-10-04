/* Simple server for archive data.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
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
#include "transform.h"
#include "disk.h"

#include "socket_server.h"


/* String used to report protocol version in response to CV command. */
#define PROTOCOL_VERSION    "0"


static bool __attribute__((format(printf, 2, 3)))
    write_string(int sock, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *buffer = NULL;
    bool ok =
        TEST_IO(vasprintf(&buffer, format, args))  &&
        TEST_write_(
            sock, buffer, strlen(buffer), "Unable to write response");
    free(buffer);
    return ok;
}


static double get_mean_frame_rate(void)
{
    const struct disk_header *header = get_header();
    return (1e6 * header->major_sample_count) / header->last_duration;
}


/* The C command prefix is followed by a sequence of one letter commands, and
 * each letter receives a one line response.  The following commands are
 * supported:
 *
 *  Q   Closes archive server
 *  H   Halts data capture (only sensible for debug use)
 *  R   Resumes halted data capture
 *
 *  F   Returns current sample frequency
 *  d   Returns first decimation
 *  D   Returns second decimation
 *  T   Returns earliest available timestamp
 *  V   Returns protocol identification string
 */
static bool process_command(int scon, const char *buf)
{
    const struct disk_header *header = get_header();
    bool ok = true;
    for (buf ++; ok  &&  *buf != '\0'; buf ++)
    {
        switch (*buf)
        {
            case 'Q':
                log_message("Shutdown command received");
                ok = write_string(scon, "Shutdown\n");
                shutdown_archiver();
                break;
            case 'H':
                log_message("Temporary halt command received");
                ok = write_string(scon, "Halted\n");
                enable_buffer_write(false);
                break;
            case 'R':
                log_message("Resume command received");
                enable_buffer_write(true);
                break;

            case 'F':
                ok = write_string(scon, "%f\n", get_mean_frame_rate());
                break;
            case 'd':
                ok = write_string(scon,
                     "%"PRIu32"\n", header->first_decimation);
                break;
            case 'D':
                ok = write_string(scon,
                     "%"PRIu32"\n", header->second_decimation);
                break;
            case 'V':
                ok = write_string(scon, PROTOCOL_VERSION "\n");
                break;

            default:
                ok = write_string(scon, "Unknown command '%c'\n", *buf);
                break;
        }
    }
    return ok;
}


/* A subscribe request is either S<mask> or SR<raw-mask>. */
static bool parse_subscription(
    const char **string, filter_mask_t mask, bool *want_timestamp)
{
    return
        parse_char(string, 'S')  &&
        parse_mask(string, mask)  &&
        DO_(*want_timestamp = read_char(string, 'T'));
}


bool report_socket_error(int scon, bool ok)
{
    bool write_ok;
    if (ok)
    {
        /* If all is well write a single null to let the caller know to expect a
         * normal response to follow. */
        pop_error_handling(NULL);
        char nul = '\0';
        write_ok = TEST_write(scon, &nul, 1);
    }
    else
    {
        /* If an error is encountered write the error message to the socket. */
        char *error_message;
        pop_error_handling(&error_message);
        write_ok = write_string(scon, "%s\n", error_message);
        free(error_message);
    }
    return write_ok;
}


/* A subscription is a command of the form S<mask> where <mask> is a mask
 * specification as described in mask.h.  The default mask is empty. */
static bool process_subscribe(int scon, const char *buf)
{
    filter_mask_t mask;
    push_error_handling();
    bool want_timestamp = false;
    bool parse_ok = DO_PARSE(
        "subscription", parse_subscription, buf, mask, &want_timestamp);
    bool ok = report_socket_error(scon, parse_ok);

    if (parse_ok  &&  ok)
    {
        struct reader_state *reader = open_reader(false);
        struct timespec ts;
        const void *block = get_read_block(reader, NULL, &ts);
        ok = TEST_NULL_(block, "No data currently available");
        if (ok  &&  want_timestamp)
        {
            uint64_t timestamp =
                (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            ok = TEST_write(scon, &timestamp, sizeof(uint64_t));
        }

        while (ok)
        {
            ok = FINALLY(
                write_frames(scon, mask, block, fa_block_size / FA_FRAME_SIZE),
                // Always do this, even if write_frames fails.
                TEST_OK_(release_read_block(reader),
                    "Write underrun to client"));
            if (ok)
                ok = TEST_NULL_(
                    block = get_read_block(reader, NULL, NULL),
                    "Gap in subscribed data");
            else
                block = NULL;
        }
        if (block != NULL)
            release_read_block(reader);

        close_reader(reader);
    }
    return ok;
}


static bool process_error(int scon, const char *buf)
{
    return write_string(scon, "Invalid command\n");
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

    /* Retrieve client address so we can log all messages associated with this
     * client with the appropriate address. */
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    char client_name[64];
    if (TEST_IO(getpeername(scon, (struct sockaddr *) &name, &namelen)))
    {
        uint8_t * ip = (uint8_t *) &name.sin_addr.s_addr;
        sprintf(client_name, "%u.%u.%u.%u:%u",
            ip[0], ip[1], ip[2], ip[3], ntohs(name.sin_port));
    }
    else
        sprintf(client_name, "unknown");

    /* Read the command, required to be one line terminated by \n, and dispatch
     * to the appropriate handler.  Any errors are handled locally and are
     * reported below. */
    char buf[4096];
    push_error_handling();
    bool ok = read_line(scon, buf, sizeof(buf));
    if (ok)
    {
        log_message("Client %s: \"%s\"", client_name, buf);
        /* Command successfully read, dispatch it to the appropriate handler. */
        struct command_table * command = command_table;
        while (command->id  &&  command->id != buf[0])
            command += 1;

        ok = command->process(scon, buf);
    }
    ok = TEST_IO(close(scon))  &&  ok;

    /* Report any errors. */
    char *error_message;
    pop_error_handling(&error_message);
    if (!ok)
        log_message("Client %s: %s", client_name, error_message);
    free(error_message);

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
