/* Command line tool to capture stream of FA sniffer data to file. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
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
#include <math.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "matlab.h"
#include "parse.h"


#define DEFAULT_SERVER      "fa-archiver.cs.diamond.ac.uk"
#define BUFFER_SIZE         (1 << 16)
#define PROGRESS_INTERVAL   (1 << 18)


enum data_format { DATA_FA, DATA_D, DATA_DD };

/* Command line parameters. */
static int port = 8888;
static const char *server_name = DEFAULT_SERVER;
static const char *output_filename = NULL;
static filter_mask_t capture_mask;
static bool matlab_format = true;
static bool squeeze_matlab = true;
static bool continuous_capture = false;
static bool start_specified = false;
static struct timespec start;
static unsigned int sample_count = 0;
static enum data_format data_format = DATA_FA;
static unsigned int data_mask = 1;
static bool show_progress = true;
static bool request_contiguous = false;
static const char *data_name = "data";

/* Archiver parameters read from archiver during initialisation. */
static double sample_frequency;
static int first_decimation;
static int second_decimation;

static int output_file = STDOUT_FILENO;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Server connection core. */


/* Connnects to the server. */
static bool connect_server(int *sock)
{
    struct sockaddr_in s_in = {
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
            &s_in.sin_addr.s_addr, hostent->h_addr, hostent->h_length))  &&
        TEST_IO(*sock = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO_(
            connect(*sock, (struct sockaddr *) &s_in, sizeof(s_in)),
            "Unable to connect to server %s:%d", server_name, port);
}


/* Reads a complete (short) response from the server until end of input, fails
 * if buffer overflows (or any other reason). */
static bool read_response(int sock, char *buf, size_t buflen)
{
    ssize_t rx;
    bool ok;
    while (
        ok =
            TEST_OK_(buflen > 0, "Read buffer exhausted")  &&
            TEST_IO(rx = read(sock, buf, buflen)),
        ok  &&  rx > 0)
    {
        buflen -= rx;
        buf += rx;
    }
    if (ok)
        *buf = '\0';
    return ok;
}


static bool parse_archive_parameters(const char **string)
{
    return
        parse_double(string, &sample_frequency)  &&
        parse_char(string, '\n')  &&
        parse_int(string, &first_decimation)  &&
        parse_char(string, '\n')  &&
        parse_int(string, &second_decimation)  &&
        parse_char(string, '\n');
}

static bool read_archive_parameters(void)
{
    int sock;
    char buffer[64];
    return
        connect_server(&sock)  &&
        TEST_write(sock, "CFdD\n", 5)  &&
        FINALLY(
            read_response(sock, buffer, sizeof(buffer)),
            // Finally, whether read_response succeeds
            TEST_IO(close(sock)))  &&
        DO_PARSE("server response", parse_archive_parameters, buffer);
}


static unsigned int get_decimation(void)
{
    switch (data_format)
    {
        /* Deliberate fall-through in all three cases. */
        case DATA_DD:   return first_decimation * second_decimation;
        case DATA_D:    return first_decimation;
        case DATA_FA:   return 1;
        default:        return 0;   // Not going to happen
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Argument parsing. */


static void usage(char *argv0)
{
    printf(
"Usage: %s [options] <capture-mask> [<samples>]\n"
"\n"
"Captures sniffer frames from the FA archiver, either reading historical data\n"
"(if -b, -s or -t is given) or live continuous data (if -C is specified).\n"
"\n"
"<capture-mask> specifies precisely which BPM ids will be captured.\n"
"The mask is specified as a comma separated sequence of ranges or BPM ids\n"
"where a range is two BPM ids separated by a hyphen, ie:\n"
"    mask = id [ \"-\" id ] [ \",\" mask ]\n"
"For example, 1-168 specifies all arc BPMs.\n"
"\n"
"<samples> specifies how many samples will be captured or the sample time in\n"
"seconds (if the number ends in s).  This must be specified when reading\n"
"historical data (-b, -s or -t).  If <samples> is not specified continuous\n"
"capture (-C) can be interrupted with ctrl-C.\n"
"\n"
"Either a start time or continuous capture must be specified, and so\n"
"precisely one of the following must be given:\n"
"   -s:  Specify start, as a date and time in ISO 8601 format (with\n"
"        fractional seconds allowed), interpreted as a time in UTC.\n"
"   -t:  Specify start as a time of day today, or yesterday if Y added to\n"
"        the end, in format hh:mm:ss[Y], interpreted as a local time.\n"
"   -b:  Specify start as a time in the past as hh:mm:ss\n"
"   -C   Request continuous capture from live data stream\n"
"\n"
"The following options can be given:\n"
"\n"
"   -o:  Save output to specified file, otherwise stream to stdout\n"
"   -f:  Specify data format, can be -fF for FA (the default), -fd[mask] for\n"
"        single decimated data, or -fD[mask] for double decimated data, where\n"
"        [mask] is an optional data mask, default value 7 (all fields).\n"
"        Decimated data is only available for archived data.\n"
"           The bits in the data mask correspond to decimated fields:\n"
"            1 => mean, 2 => min, 4 => max\n"
"   -R   Save in raw format, otherwise the data is saved in matlab format\n"
"   -c   Forbid any gaps in the captured sequence, contiguous data only\n"
"   -k   Keep extra dimensions in matlab values\n"
"   -n:  Specify name of data array (default is \"%s\")\n"
"   -S:  Specify archive server to read from (default is\n"
"            %s)\n"
"   -p:  Specify port to connect to on server (default is %d)\n"
"   -q   Suppress display of progress of capture on stderr\n"
"\n"
"Note that if matlab format is specified and continuous capture is\n"
"interrupted then output must be directed to a file, otherwise the capture\n"
"count in the result will be invalid.\n"
    , argv0, data_name, server_name, port);
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


static bool parse_today(const char **string, struct timespec *ts)
{
    return
        parse_time(string, ts)  &&
        DO_(start.tv_sec += midnight_today())  &&
        IF_(read_char(string, 'Y'),
            DO_(start.tv_sec -= 24 * 3600));
}


static bool parse_data_format(const char **string, enum data_format *format)
{
    if (read_char(string, 'F'))
    {
        *format = DATA_FA;
        return true;
    }
    else
    {
        if (read_char(string, 'd'))
            *format = DATA_D;
        else if (read_char(string, 'D'))
            *format = DATA_DD;
        else
            return FAIL_("Invalid data format");

        if (**string == '\0')
        {
            data_mask = 7;          // Read all fields by default
            return true;
        }
        else
            return
                parse_uint(string, &data_mask)  &&
                TEST_OK_(0 < data_mask  &&  data_mask <= 7,
                    "Invalid data mask");
    }
}


static bool parse_before(const char **string, struct timespec *ts)
{
    return
        parse_time(string, ts)  &&
        DO_(ts->tv_sec = time(NULL) - ts->tv_sec);
}


static bool parse_start(
    bool (*parser)(const char **string, struct timespec *ts),
    const char *string)
{
    return
        TEST_OK_(!start_specified, "Start already specified")  &&
        DO_PARSE("start time", parser, string, &start)  &&
        DO_(start_specified = true);
}



static bool parse_opts(int *argc, char ***argv)
{
    char *argv0 = (*argv)[0];
    char *argv0slash = strrchr(argv0, '/');
    if (argv0slash != NULL)
        argv0 = argv0slash + 1;

    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hRCo:S:qckn:s:t:b:p:f:"))
        {
            case 'h':   usage(argv0);                               exit(0);
            case 'R':   matlab_format = false;                      break;
            case 'C':   continuous_capture = true;                  break;
            case 'o':   output_filename = optarg;                   break;
            case 'S':   server_name = optarg;                       break;
            case 'q':   show_progress = false;                      break;
            case 'c':   request_contiguous = true;                  break;
            case 'k':   squeeze_matlab = false;                     break;
            case 'n':   data_name = optarg;                         break;
            case 's':   ok = parse_start(parse_datetime, optarg);   break;
            case 't':   ok = parse_start(parse_today, optarg);      break;
            case 'b':   ok = parse_start(parse_before, optarg);     break;
            case 'p':
                ok = DO_PARSE("server port", parse_int, optarg, &port);
                break;
            case 'f':
                ok = DO_PARSE("data format",
                    parse_data_format, optarg, &data_format);
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
        *result = (unsigned int) round(
            *result * sample_frequency / get_decimation());
    return ok  &&  TEST_OK_(*result > 0, "Zero sample count");
}


static bool parse_args(int argc, char **argv)
{
    return
        parse_opts(&argc, &argv)  &&
        TEST_OK_(argc == 1  ||  argc == 2,
            "Wrong number of arguments.  Try `capture -h` for help.")  &&
        DO_PARSE("capture mask", parse_mask, argv[0], capture_mask)  &&
        read_archive_parameters()  &&
        IF_(argc == 2,
            DO_PARSE("sample count", parse_samples, argv[1], &sample_count));
}


static bool validate_args(void)
{
    return
        TEST_OK_(continuous_capture || start_specified,
            "Must specify a start date or continuous capture")  &&
        TEST_OK_(!continuous_capture || !start_specified,
            "Cannot combine continuous and archive capture")  &&
        TEST_OK_(continuous_capture  ||  sample_count > 0,
            "Must specify sample count for historical data")  &&
        TEST_OK_(start_specified  ||  data_format == DATA_FA,
            "Decimated data must be historical");
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture */


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
    format_raw_mask(capture_mask, raw_mask);
    char request[1024];
    if (continuous_capture)
        sprintf(request, "SR%s%s\n", raw_mask, matlab_format ? "T" : "");
    else
    {
        char format[16];
        switch (data_format)
        {
            case DATA_FA:   sprintf(format, "F");                   break;
            case DATA_D:    sprintf(format, "DF%u",  data_mask);    break;
            case DATA_DD:   sprintf(format, "DDF%u", data_mask);    break;
        }
        sprintf(request, "R%sMR%sS%ld.%09ldN%u%s%s\n",
            format, raw_mask, start.tv_sec, start.tv_nsec, sample_count,
            matlab_format ? "TG" : "", request_contiguous ? "C" : "");
    }
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
            if (read_response(sock, response + 1, sizeof(response) - 1))
                fprintf(stderr, "%s", response);
            return false;
        }
    }
    else
        return false;
}



static void update_progress(unsigned int frames_written, size_t frame_size)
{
    const char *progress = "|/-\\";
    static uint64_t last_update = 0;
    uint64_t bytes_written = (uint64_t) frame_size * frames_written;
    if (bytes_written >= last_update + PROGRESS_INTERVAL)
    {
        fprintf(stderr, "%c %9d",
            progress[(bytes_written / PROGRESS_INTERVAL) % 4], frames_written);
        if (sample_count > 0)
            fprintf(stderr, " (%5.2f%%)",
                100.0 * (double) frames_written / sample_count);
        fprintf(stderr, "\r");
        fflush(stderr);
        last_update = bytes_written;
    }
}

/* Erases residue of progress marker on command line. */
static void reset_progress(void)
{
    char spaces[40];
    memset(spaces, ' ', sizeof(spaces));
    fprintf(stderr, "%.*s\r", (int) sizeof(spaces), spaces);
}


/* This routine reads data from sock and writes out complete frames until either
 * the sample count is reached or the read is interrupted. */
static unsigned int capture_data(int sock)
{
    size_t frame_size =
        count_data_bits(data_mask) *
        count_mask_bits(capture_mask) * FA_ENTRY_SIZE;
    unsigned int frames_written = 0;
    char buffer[BUFFER_SIZE];
    unsigned int residue = 0;   // Partial frame received, not yet written out
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

        if (show_progress)
            update_progress(frames_written, frame_size);
    }

    if (show_progress)
        reset_progress();
    return frames_written;
}



/* Gap list read from server. */
#define MAX_GAP_COUNT   128     // Sanity limit
static uint32_t gap_count;
static uint32_t data_index[MAX_GAP_COUNT];
static uint32_t id_zero[MAX_GAP_COUNT];
static double gap_timestamps[MAX_GAP_COUNT];

static bool read_gap_list(int sock)
{
    bool ok =
        TEST_read(sock, &gap_count, sizeof(uint32_t))  &&
        TEST_OK_(gap_count < MAX_GAP_COUNT,
            "Implausible gap count of %"PRIu32" rejected", gap_count);
    if (ok)
    {
        for (unsigned int i = 0; ok && i <= gap_count; i ++)
        {
            struct {
                uint32_t data_index;
                uint32_t id_zero;
                uint64_t timestamp;
            } gap_data;

            ok = TEST_read(sock, &gap_data, sizeof(gap_data));
            data_index[i] = gap_data.data_index;
            id_zero[i] = gap_data.id_zero;
            gap_timestamps[i] = matlab_timestamp(gap_data.timestamp);
        }
    }
    return ok;
}


static bool write_header(unsigned int frames_written, uint64_t timestamp)
{
    bool squeeze[4] = {
        false,                                      // X, Y
        data_format == DATA_FA || squeeze_matlab,   // Decimated subfield
        squeeze_matlab,                             // BPM ID
        false                                       // Sample number
    };
    uint32_t decimation = get_decimation();
    double m_timestamp = matlab_timestamp(timestamp);
    double frequency = sample_frequency / decimation;

    char mat_header[4096];
    int32_t *h = (int32_t *) mat_header;
    prepare_matlab_header(&h, sizeof(mat_header));

    /* Write out the decimation, sample frequency and timestamp. */
    place_matlab_value(&h, "decimation", miINT32, &decimation);
    place_matlab_value(&h, "f_s",        miDOUBLE, &frequency);
    place_matlab_value(&h, "timestamp",  miDOUBLE, &m_timestamp);

    /* Write out the index array tying data back to original BPM ids. */
    uint8_t mask_ids[FA_ENTRY_COUNT];
    int mask_length = compute_mask_ids(mask_ids, capture_mask);
    place_matlab_vector(&h, "ids", miUINT8, mask_ids, mask_length);

    /* Write out the gap list data. */
    place_matlab_vector(&h, "gapix", miINT32, data_index, gap_count + 1);
    place_matlab_vector(&h, "id0",   miINT32, id_zero, gap_count + 1);
    place_matlab_vector(&h, "gaptimes",
        miDOUBLE, gap_timestamps, gap_count + 1);

    /* Finally write out the matrix mat_header for the fa data. */
    int field_count = count_data_bits(data_mask);
    place_matrix_header(&h, data_name,
        miINT32, squeeze,
        FA_ENTRY_SIZE * field_count * mask_length * frames_written,
        4, 2, field_count, mask_length, frames_written);

    ASSERT_OK((char *) h < mat_header + sizeof(mat_header));
    return TEST_write(output_file, mat_header, (char *) h - mat_header);
}


static bool capture_and_save(int sock)
{
    if (matlab_format)
    {
        unsigned int frames_written;
        uint64_t timestamp;
        return
            TEST_read(sock, &timestamp, sizeof(uint64_t))  &&
            read_gap_list(sock)  &&
            write_header(sample_count, timestamp)  &&
            DO_(frames_written = capture_data(sock))  &&
            IF_(frames_written != sample_count,
                /* For an incomplete capture, probably an interrupted continuous
                 * capture, we need to rewrite the header with the correct
                 * capture count. */
                TEST_IO_(lseek(output_file, 0, SEEK_SET),
                    "Cannot update matlab file, file not seekable")  &&
                write_header(frames_written, timestamp));
    }
    else
    {
        unsigned int frames_written = capture_data(sock);
        return TEST_OK_(frames_written == sample_count,
            "Only captured %u of %u frames", frames_written, sample_count);
    }
}


int main(int argc, char **argv)
{
    char *server = getenv("FA_ARCHIVE_SERVER");
    if (server != NULL)
        server_name = server;

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
