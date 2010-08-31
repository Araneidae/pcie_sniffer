/* Command line tool to capture stream of FA sniffer data to file. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "matlab.h"


#define BUFFER_SIZE     (1 << 16)


/* Command line parameters. */
static int port = 8888;
static const char *server_name;
static const char *output_filename = NULL;
static filter_mask_t mask;
static bool matlab_format = false;
static unsigned int sample_count = 0;

static int output_file = STDOUT_FILENO;


static void usage(void)
{
    printf(
"Usage: capture [options] <archiver> <capture-mask>\n"
"\n"
"Captures a continuous stream of sniffer frames from <archiver> until\n"
"interrupted or the specified number of samples have been captured.\n"
"The <capture-mask> specifies precisely which BPM ids will be captured.\n"
"\n"
"The following options can be given:\n"
"\n"
"   -p:  Specify port to connect to on server (default is %d)\n"
"   -n:  Specify number of samples or the sample time in seconds (if the\n"
"        number ends in s).  Otherwise interrupt to stop capture.\n"
"   -M   Save file in matlab format\n"
"   -o:  Save output to specified file, otherwise stream to stdout\n"
"\n"
"Note that if -M is specified and capture is interrupted then output must\n"
"be directed to a file, otherwise the capture count in the result will\n"
"be invalid.\n"
    , port);
}


static bool parse_int(const char *string, int *result)
{
    char *end;
    *result = strtol(string, &end, 10);
    return TEST_OK_(end > string  &&  *end == 0,
        "Malformed number: \"%s\"", string);
}


static bool parse_samples(const char *string, unsigned int *result)
{
    char *end;
    *result = strtoul(string, &end, 10);
    if (end > string  &&  *end == 's')
    {
        *result *= 10072;       // Convert seconds to samples
        end ++;
    }
    return TEST_OK_(end > string  &&  *end == 0,
        "Malformed number: \"%s\"", string);
}


static bool parse_opts(int *argc, char ***argv)
{
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hp:n:Mo:"))
        {
            case 'h':   usage();                                    exit(0);
            case 'p':   ok = parse_int(optarg, &port);              break;
            case 'n':   ok = parse_samples(optarg, &sample_count);  break;
            case 'M':   matlab_format = true;                       break;
            case 'o':   output_filename = optarg;                   break;
            default:
                fprintf(stderr, "Try `capture -h` for usage\n");
                return false;
            case -1:
                *argc -= optind;
                *argv += optind;
                return true;
        }
    }
    return false;
}


static bool parse_args(int argc, char **argv)
{
    return
        parse_opts(&argc, &argv)  &&
        TEST_OK_(argc == 2,
            "Wrong number of arguments.  Try `capture -h` for help.")  &&
        DO_(server_name = argv[0])  &&
        parse_mask(argv[1], mask, NULL);
}


static bool connect_server(int *sock)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    struct hostent *hostent;
    return
        TEST_NULL_(
            hostent = gethostbyname(server_name),
            "Unable to resolve server \"%s\"", server_name)  &&
        DO_(memcpy(
            &sin.sin_addr.s_addr, hostent->h_addr, hostent->h_length))  &&
        TEST_IO(*sock = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO_(
            connect(*sock, (struct sockaddr *) &sin, sizeof(sin)),
            "Unable to connect to server %s:%d", server_name, port);
}


static volatile bool running = true;

static void interrupt_capture(int signum)
{
    running = false;
}


static bool initialise_signal(void)
{
    struct sigaction interrupt = {
        .sa_handler = interrupt_capture, .sa_flags = 0 };
    struct sigaction do_ignore   = { .sa_handler = SIG_IGN, .sa_flags = 0 };
    return
        TEST_IO(sigfillset(&interrupt.sa_mask))  &&
        TEST_IO(sigaction(SIGINT,  &interrupt, NULL))  &&
        TEST_IO(sigaction(SIGPIPE, &do_ignore, NULL));
}


static bool request_data(int sock)
{
    char request[RAW_MASK_BYTES + 8];
    strcpy(request, "SR");
    format_raw_mask(mask, request + 2);
    return TEST_write(sock, request, RAW_MASK_BYTES + 2);
}


/* This routine reads data from sock and writes out complete frames until either
 * the sample count is reached or the read is interrupted. */
static unsigned int capture_data(int sock)
{
    int frame_size = count_mask_bits(mask) * FA_ENTRY_SIZE;
    unsigned int frames_written = 0;
    char buffer[BUFFER_SIZE];
    int residue = 0;            // Partial frame received, not yet written out
    while (running  &&  (sample_count == 0  ||  frames_written < sample_count))
    {
        int rx = read(sock, buffer + residue, BUFFER_SIZE - residue);
        if (rx == -1)
        {
            TEST_OK_(errno == EINTR, "Error reading from server");
            break;
        }
        else if (rx == 0)
            break;

        rx = rx + residue;
        unsigned int frames_read = rx / frame_size;
        if (sample_count > 0  &&  frames_read > (sample_count - frames_written))
            frames_read = sample_count - frames_written;
        unsigned int to_write = frames_read * frame_size;
        if (frames_read > 0)
        {
            if (!TEST_write(output_file, buffer, to_write))
                break;
            frames_written += frames_read;
        }

        /* For lazy simplicity just move any unwritten partial frames to the
         * bottom of the buffer. */
        residue = rx - to_write;
        if (residue > 0)
            memmove(buffer, buffer + to_write, residue);
    }

    return frames_written;
}


static bool capture_and_save(int sock)
{
    if (matlab_format)
    {
        unsigned int frames_written;
        return
            write_matlab_header(output_file, mask, sample_count, true)  &&
            DO_(frames_written = capture_data(sock))  &&
            IF_(frames_written != sample_count,
                /* Seek back to the start of the file and rewrite the header
                 * with the correct capture count. */
                TEST_IO_(lseek(output_file, 0, SEEK_SET),
                    "Cannot update matlab file, file not seekable")  &&
                write_matlab_header(output_file, mask, frames_written, true));
    }
    else
        return DO_(capture_data(sock));
}


int main(int argc, char **argv)
{
    if (!parse_args(argc, argv))
        return 1;

    int sock;
    bool ok =
        connect_server(&sock)  &&
        IF_(output_filename != NULL,
            TEST_IO_(
                output_file = open(
                    output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0666),
                "Unable to open output file \"%s\"", output_filename))  &&
        request_data(sock)  &&
        initialise_signal()  &&
        capture_and_save(sock);
    return ok ? 0 : 2;
}
