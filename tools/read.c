/* Utility to extract archived data from an archiver file. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

#include "error.h"
#include "disk.h"
#include "mask.h"


#define USAGE_STRING \
    "Usage: read [options] <archive-file>\n" \
    "Reads stuff ...\n" \
    "Options:\n" \
    "   -m: Select BPM ids to be read from archive\n" \
    "   -H  Show header information\n" \
    "   -h  Shows this help text\n" 



static filter_mask_t filter_mask;

static char * file_name;
static int file_fd;

static struct disk_header header;
static struct disk_header *header_mmap;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Argument processing.                                                      */

static int file_out = STDOUT_FILENO;
static bool show_header = false;           // Header mode selected
static int dump_length = 0;                // Number of frames to dump
static int dump_end = 0;                   // End offset of frames
static bool show_progress = true;          // Show progress bar
static bool pace_progress = true;          // Pace progress to avoid gapping
static char * argv0;
static bool absolute_offset = false;


static void usage(void)
{
    printf(
"Usage, one of:\n"
"    read [options] <archive-file> <frame-count> [<end-offset>]\n"
"        Read frames from archive file.\n"
"    read -H <archive-file>\n"
"        Read header of archive file.\n"
"    read -h\n"
"        Show this help text\n"
"\n"
"Interrogates status of FA sniffer archive and extracts data to file.\n"
"\n"
"Options:\n"
"   -a  Specified end frame offset is absolute offset into archive\n"
"   -m: Select BPM ids to be read from archive, default is to read all 256\n"
"   -o: Specify output file (otherwise written to stdout)\n"
"   -q  Don't show progress while reading frames\n"
"   -n  Don't pace reading: can result in gaps in archive\n"
"\n"
"The end frame offset is normally a negative offset from the most recently\n"
"written frame, with default 0, so the most recent frames are normally\n"
"captured.  However -a can be used to select an offset into the buffer\n"
"instead.\n"
        );
}


static bool parse_int(char *string, int *result)
{
    char *end;
    *result = strtol(string, &end, 0);
    return
        TEST_OK_(end > string,
            "No number in argument \"%s\"", string)   &&
        TEST_OK_(*end == '\0',
            "Unexpected characters after number \"%s\"", string);
}


static bool parse_opts(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hHm:o:aqn"))
        {
            case 'h':   usage();                                    exit(0);
            case 'H':   show_header = true;                         break;
            case 'm':   ok = parse_mask(optarg, filter_mask);       break;
            case 'o':
                ok = TEST_IO_(
                    file_out = open(
                        optarg, O_WRONLY | O_CREAT | O_TRUNC, 0666),
                    "Unable to open output file \"%s\"", optarg);
                break;
            case 'a':   absolute_offset = true;                     break;
            case 'q':   show_progress = false;                      break;
            case 'n':   pace_progress = false;                      break;

            case '?':
            default:
                fprintf(stderr, "Try `%s -h` for usage\n", argv0);
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
#define PROCESS_ARG(missing, action) \
    ( TEST_OK_(argc >= 1, missing " argument missing")  && \
      action  && \
      DO_(argc -= 1; argv += 1) )

    if (argc == 0)
    {
        fprintf(stderr, "Try -h for usage\n");
        return false;
    }
    else
    {
        dump_end = 0;
        return
            PROCESS_ARG("Filename",
                DO_(file_name = *argv))  &&
            IF_(!show_header,
                PROCESS_ARG("Frame count",
                    parse_int(*argv, &dump_length))  &&
                IF_(argc > 0,
                    PROCESS_ARG("Frame offset",
                        parse_int(*argv, &dump_end))))  &&
            TEST_OK_(argc == 0, "Too many arguments");
    }
#undef PROCESS_ARG
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data processing.                                                          */


static void read_one_frame(void)
{
    char read_buffer[FA_FRAME_SIZE];
    char write_buffer[FA_FRAME_SIZE];
    ASSERT_read(file_fd, read_buffer, FA_FRAME_SIZE);
    size_t n = copy_frame(write_buffer, read_buffer, filter_mask);
    ASSERT_write(file_out, write_buffer, n);
}


static void read_header(void)
{
    struct flock flock = {
        .l_type = F_RDLCK, .l_whence = SEEK_SET,
        .l_start = 0, .l_len = sizeof(header)
    };

    ASSERT_IO(fcntl(file_fd, F_SETLKW, &flock));
    memcpy(&header, header_mmap, DISK_HEADER_SIZE);
    flock.l_type = F_UNLCK;
    ASSERT_IO(fcntl(file_fd, F_SETLK, &flock));
}


static void seek_data(int64_t dump_offset)
{
    ASSERT_IO(lseek(file_fd,
        header.h.data_start + dump_offset * FA_FRAME_SIZE, SEEK_SET));
}


static uint64_t get_now(void)
{
    struct timespec ts;
    ASSERT_IO(clock_gettime(CLOCK_REALTIME, &ts));
    return (unsigned) ts.tv_sec;
}


#define PROGRESS_INTERVAL   (1 << 12)

static void update_progress(int n, int final_n)
{
    const char *progress = "|/-\\";
    if (n % PROGRESS_INTERVAL == 0)
    {
        fprintf(stderr, "%c %9d (%5.2f%%)                \r",
            progress[(n / PROGRESS_INTERVAL) % 4], n,
            100.0 * (double) n / final_n);
        fflush(stderr);
    }
}


static void pace_reading(int64_t dump_offset, uint64_t *now)
{
    uint64_t tick = get_now();
    if (tick != *now)
    {
        *now = tick;
        read_header();

        /* Convert the backlog into seconds (rounding down) and sleep for that
         * long if necessary. */
        int backlog = (header.h.write_backlog / FA_FRAME_SIZE) / 10000;
        if (backlog > 0)
        {
            if (show_progress)
                fprintf(stderr, "Backlog %d, pausing for %ds\r",
                    header.h.write_backlog / FA_FRAME_SIZE, backlog);
            sleep(backlog);
        }
    }
}


static void dump_data(void)
{
    /* Length of the data area in frames. */
    int64_t data_length = header.h.data_size / FA_FRAME_SIZE;
    /* If the end offset is relative, update it relative to the end of the
     * most recently captured block. */
    if (!absolute_offset)
    {
        dump_end = header.blocks[0].stop_offset / FA_FRAME_SIZE - dump_end;
        if (dump_end < 0)
            dump_end += data_length;
    }

    /* Similarly compute the start. */
    int64_t dump_offset = dump_end - dump_length;
    if (dump_offset < 0)
        dump_offset += data_length;


    /* Now let's be quite simple minded, go block by block. */
    seek_data(dump_offset);
    uint64_t now = get_now();
    for (int i = 0; i < dump_length; i ++)
    {
        read_one_frame();
        dump_offset += 1;
        if (dump_offset >= data_length)
        {
            dump_offset -= data_length;
            seek_data(dump_offset);
        }
        
        if (pace_progress)
            pace_reading(dump_offset, &now);
        if (show_progress)
            update_progress(i, dump_length);
    }
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation.                                                           */


static bool prepare_header(void)
{
    uint64_t file_size;
    return
        TEST_IO(
            header_mmap = mmap(0, DISK_HEADER_SIZE,
                PROT_READ, MAP_SHARED, file_fd, 0))  &&
        DO_(read_header())  &&
        get_filesize(file_fd, &file_size)  &&
        validate_header(&header, file_size);
}


int main(int argc, char **argv)
{
    memset(filter_mask, 0xff, sizeof(filter_mask_t));
    
    if (parse_opts(&argc, &argv)  &&  parse_args(argc, argv))
    {
        bool ok =
            TEST_IO_(
                file_fd = open(file_name, O_RDONLY),
                "Unable to open file \"%s\"", file_name)  &&
            prepare_header();

        if (ok)
        {
            if (show_header)
            {
                FILE *out;
                ASSERT_NULL(out = fdopen(file_out, "w"));
                print_header(out, &header);
            }
            else
                dump_data();
        }
        
        return ok ? 0 : 2;
    }
    else
        return 1;
}
