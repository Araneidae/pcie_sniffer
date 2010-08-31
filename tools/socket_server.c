/* Simple server for archive data.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
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
#include "socket_server.h"


static void write_string(int sock, const char *string)
{
    write(sock, string, strlen(string));
}


static void process_http(int scon, char *buf, ssize_t rx)
{
    write_string(scon,
        "HTTP/1.0 200 OK\r\n\r\n<HTML><BODY>Ok!</BODY></HTML>\r\n");
}


/* We provide a very limited set of commands:
 *  CQ      Closes archive server
 *  CS      Prints a simple status report
 *  CF      Returns current sample frequency
 */
static void process_command(int scon, char *buf, ssize_t rx)
{
    if (rx >= 2)
    {
        switch (buf[1])
        {
            case 'Q':
                log_message("Shutdown command received");
                shutdown_archiver();
                break;
            case 'S':
                write_string(scon, "status not implemented\n");
                break;
            case 'F':
            {
                char message[20];
                sprintf(message, "%f", get_mean_frame_rate());
                write_string(scon, message);
                break;
            }
            default:
                write_string(scon, "Unknown command\n");
        }
    }
    else
        write_string(scon, "Missing command\n");
}


static void process_read(int scon, char *buf, ssize_t rx)
{
    write_string(scon, "Not implemented\n");
}


/* A subscription is a command of the form S<mask> where <mask> is a mask
 * specification as described in mask.h.  The default mask is empty. */
static void process_subscribe(int scon, char *buf, ssize_t rx)
{
    filter_mask_t mask;
    /* A subscribe request is either S<mask> or SR<raw-mask>. */
    bool parse_ok = IF_ELSE(
        buf[1] == 'R',
            parse_raw_mask(buf + 2, mask),
            parse_mask(buf + 1, mask));
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
}


static void process_error(int scon, char *buf, ssize_t rx)
{
    write_string(scon, "Invalid command\n");
}


struct command_table {
    char id;            // Identification character
    void (*process)(int scon, char *buf, ssize_t rx);
} command_table[] = {
    { 'G', process_http },
    { 'C', process_command },
    { 'R', process_read },
    { 'S', process_subscribe },
    { 0,   process_error }
};


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

    char buf[4096];
    ssize_t rx;
    memset(buf, 0, sizeof(buf));
    if (TEST_IO(rx = read(scon, buf, sizeof(buf)))  &&  rx > 0)
    {
        log_message("Read: \"%.*s\"", (int)rx, buf);
        /* Command successfully read, dispatch it to the appropriate handler. */
        struct command_table * command = command_table;
        while (command->id  &&  command->id != buf[0])
            command += 1;
        command->process(scon, buf, rx);
    }
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
