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
#include <time.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "matlab.h"
#include "parse.h"


#define BUFFER_SIZE     (1 << 16)

enum data_format { DATA_FA, DATA_D, DATA_DD };

/* Command line parameters. */
static int port = 8888;
static const char *server_name;
static const char *output_filename = NULL;
static filter_mask_t mask;
static bool matlab_format = false;
static bool continuous_capture = false;
static bool start_specified = false;
static struct timespec start;
static unsigned int sample_count = 0;
static enum data_format data_format = DATA_FA;

static int output_file = STDOUT_FILENO;


static void usage(void)
{
    printf(
"Usage: capture [options] <archiver> <capture-mask> [<samples>]\n"
"\n"
"Captures sniffer frames from <archiver>, either reading historical data\n"
"(if -s or -t is used) or live continuous data (if -C is specified).\n"
"\n"
"<capture-mask> specifies precisely which BPM ids will be captured.\n"
"The mask is specified as a comma separated sequence of ranges or BPM ids\n"
"where a range is two BPM ids separated by a hyphen, eg\n"
"    mask = id [ \"-\" id ] [ \",\" mask ]\n"
"For example, 1-168 specifies all arc BPMs.\n"
"\n"
"<samples> specifies how many samples will be captured or the sample time in\n"
"seconds (if the number ends in s).  This must be specified when reading\n"
"historical data (-s or -t).  If <samples> is not specified continuous\n"
"capture (-C) can be interrupted with ctrl-C.\n"
"\n"
"The following options can be given:\n"
"\n"
"   -p:  Specify port to connect to on server (default is %d)\n"
"   -M   Save file in matlab format\n"
"   -o:  Save output to specified file, otherwise stream to stdout\n"
"   -s:  Specify start, as a date and time in ISO 8601 format\n"
"   -t:  Specify start as a time of day today\n"
"   -C   Request continuous capture from live data stream\n"
"   -f:  Specify data format, can be -fF for FA (the default), -fd for single\n"
"        decimated data, or -fD for double decimated data.  Decimated data is\n"
"        only avaiable for archived data.\n"
"\n"
"Either a start time or continuous capture must be specified.\n"
"Note that if -M is specified and continuous capture is interrupted then\n"
"output must be directed to a file, otherwise the capture count in the result\n"
"will be invalid.\n"
    , port);
}


/* Returns seconds at midnight this morning for time of day relative timestamp
 * specification.  This uses the current timezone.  Horrible code. */
static time_t midnight_today(void)
{
    time_t now;
    ASSERT_IO(now = time(NULL));
    struct tm tm;
    ASSERT_NULL(localtime_r(&now, &tm));
    tm.tm_sec = 0;
    tm.tm_min = 0;
    tm.tm_hour = 0;
    time_t midnight;
    ASSERT_IO(midnight = mktime(&tm));
    return midnight;
}


static bool parse_data_format(char *string)
{
    if (strcmp(string, "F") == 0)
        data_format = DATA_FA;
    else if (strcmp(string, "D") == 0)
        data_format = DATA_D;
    else if (strcmp(string, "DD") == 0)
        data_format = DATA_DD;
    else
        return TEST_OK_(false, "Invalid data format");
    return true;
}


static bool parse_opts(int *argc, char ***argv)
{
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hMoCf:p:s:t:"))
        {
            case 'h':   usage();                                    exit(0);
            case 'M':   matlab_format = true;                       break;
            case 'o':   output_filename = optarg;                   break;
            case 'C':   continuous_capture = true;                  break;
            case 'f':   ok = parse_data_format(optarg);             break;
            case 'p':
                ok = DO_PARSE("server port", parse_int, optarg, &port);
                break;
            case 's':
                start_specified = true;
                ok = DO_PARSE("start time", parse_datetime, optarg, &start);
                break;
            case 't':
                start_specified = true;
                ok = DO_PARSE("start time", parse_time, optarg, &start);
                start.tv_sec += midnight_today();
                break;
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


static bool parse_samples(const char **string, unsigned int *result)
{
    bool ok = parse_uint(string, result);
    if (ok  &&  read_char(string, 's'))
        *result *= 10072;
    return ok;
}


static bool parse_args(int argc, char **argv)
{
    return
        parse_opts(&argc, &argv)  &&
        TEST_OK_(argc == 2  ||  argc == 3,
            "Wrong number of arguments.  Try `capture -h` for help.")  &&
        DO_(server_name = argv[0])  &&
        DO_PARSE("capture mask", parse_mask, argv[1], mask)  &&
        IF_(argc == 3,
            DO_PARSE("sample count", parse_samples, argv[2], &sample_count));
}


static bool validate_args(void)
{
    return
        TEST_OK_(continuous_capture != start_specified,
            "Must specify precisely one of -s or -C")  &&
        TEST_OK_(continuous_capture  ||  sample_count > 0,
            "Must specify sample count for historical data")  &&
        TEST_OK_(start_specified  ||  data_format == DATA_FA,
            "Decimated data must be historical");
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
    struct sigaction do_ignore = { .sa_handler = SIG_IGN, .sa_flags = 0 };
    return
        TEST_IO(sigfillset(&interrupt.sa_mask))  &&
        TEST_IO(sigaction(SIGINT,  &interrupt, NULL))  &&
        TEST_IO(sigaction(SIGPIPE, &do_ignore, NULL));
}


static bool request_data(int sock)
{
    char raw_mask[RAW_MASK_BYTES+1];
    format_raw_mask(mask, raw_mask);
    char request[1024];
    if (continuous_capture)
        sprintf(request, "SR%s\n", raw_mask);
    else
        sprintf(request, "R%sR%sS%ld.%09ldN%u\n",
            "F", raw_mask, start.tv_sec, start.tv_nsec, sample_count);
    return TEST_write(sock, request, strlen(request));
}


/* If the request was accepted the first byte of the response is a null
 * character, otherwise the entire response is an error message. */
static bool check_response(int sock)
{
    char response[1024];
    if (TEST_read(sock, response, 1))
    {
        if (*response == '\0')
            return true;
        else
        {
            /* Pass entire error response from server to stderr. */
            int len;
            if (TEST_IO(len = read(sock, response + 1, sizeof(response) - 1)))
                fprintf(stderr, "%.*s", len + 1, response);
            return false;
        }
    }
    else
        return false;
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
    int sock;
    bool ok =
        parse_args(argc, argv)  &&
        validate_args()  &&

        connect_server(&sock)  &&
        IF_(output_filename != NULL,
            TEST_IO_(
                output_file = open(
                    output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0666),
                "Unable to open output file \"%s\"", output_filename))  &&
        request_data(sock)  &&
        check_response(sock)  &&

        initialise_signal()  &&
        capture_and_save(sock);
    return ok ? 0 : 1;
}
