/* Utility to extract archived data from an archiver file. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
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




static int file_fd;
static int file_out = STDOUT_FILENO;

static struct disk_header header;
static struct disk_header *header_mmap;



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Argument processing.                                                      */

static char * archive_file_name;
static char * out_file_name = NULL;
static filter_mask_t filter_mask;
static bool show_header = false;           // Header mode selected
static int dump_length = 0;                // Number of frames to dump
static int dump_end = 0;                   // End offset of frames
static bool show_progress = true;          // Show progress bar
static bool pace_progress = true;          // Pace progress to avoid gapping
static char * argv0;
static bool absolute_offset = false;
static int backoff_threshold = 20000;
static bool matlab_format = false;
static bool matlab_double = false;


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
"   -m: Select BPM ids to be read from archive, default is to read all 256.\n"
"       The mask is a series of comma separated ids or ranges, where a range\n"
"       is a pair of ids separated by -.  Eg, -m1-168,255 fetchs all BPMs\n"
"       together with the timestamp field.\n"
"   -o: Specify output file (otherwise written to stdout)\n"
"   -q  Don't show progress while reading frames\n"
"   -n  Don't pace reading: can result in gaps in archive\n"
"   -b: Specify backoff threshold (default is 20000 frames)\n"
"   -M  Write output as matlab file\n"
"   -D  Write matlab output in double format, implies -M\n"
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
        switch (getopt(*argc, *argv, "+hHm:o:aqnb:MD"))
        {
            case 'h':   usage();                                    exit(0);
            case 'H':   show_header = true;                         break;
            case 'm':   ok = parse_mask(optarg, filter_mask);       break;
            case 'o':   out_file_name = optarg;                     break;
            case 'a':   absolute_offset = true;                     break;
            case 'q':   show_progress = false;                      break;
            case 'n':   pace_progress = false;                      break;
            case 'b':   ok = parse_int(optarg, &backoff_threshold); break;
            case 'M':   matlab_format = true;                       break;
            case 'D':   matlab_format = true; matlab_double = true; break;

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
                DO_(archive_file_name = *argv))  &&
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
/* Matlab format support.                                                    */

/* The matlab format symbol definitions we use. */
#define miINT8          1
#define miUINT8         2
#define miINT32         5
#define miUINT32        6
#define miMATRIX        14

#define mxDOUBLE_CLASS  6
#define mxINT32_CLASS   12


static void compute_mask_ids(uint8_t *array, filter_mask_t mask)
{
    for (int bit = 0; bit < 256; bit ++)
        if (test_mask_bit(mask, bit))
            *array++ = bit;
}


static void write_matlab_string(int32_t **hh, char *string)
{
    int32_t *h = *hh;
    int l = strlen(string);
    *h++ = miINT8;      *h++ = l;
    memcpy(h, string, l);
    *hh = h + 2 * ((l + 7) / 8);
}


/* Returns the number of bytes of padding required after data_length bytes of
 * following data to ensure that the entire matrix is padded to 8 bytes. */
static int write_matrix_header(
    int32_t **hh, int class, char *name,
    int data_type, int data_length,
    int dimensions, ...)
{
    va_list dims;
    va_start(dims, dimensions);

    int32_t *h = *hh;
    *h++ = miMATRIX;
    int32_t *l = h++;   // total length will be written here.
    // Matrix flags: consists of two uint32 words encoding the class.
    *h++ = miUINT32;    *h++ = 8;
    *h++ = class;
    *h++ = 0;
    
    // Matrix dimensions: one int32 for each dimension
    *h++ = miINT32;     *h++ = dimensions * sizeof(int32_t);
    for (int i = 0; i < dimensions; i ++)
        *h++ = va_arg(dims, int32_t);
    h += dimensions & 1;    // Padding if required
    
    // Element name
    write_matlab_string(&h, name);
    
    // Data header: data follows directly after.
    int padding = (8 - data_length) & 7;
    *h++ = data_type;   *h++ = data_length;
    *l = data_length + (h - l - 1) * sizeof(int32_t) + padding;
    
    *hh = h;
    return padding;
}


static void write_matlab_header(void)
{
    char header[4096];
    memset(header, 0, sizeof(header));

    /* The first 128 bytes are the description and format marks. */
    memset(header, ' ', 124);
    sprintf(header, "MATLAB 5.0 MAT-file generated from FA sniffer data");
    header[strlen(header)] = ' ';
    *(uint16_t *)&header[124] = 0x0100;   // Version flag
    *(uint16_t *)&header[126] = 0x4d49;   // 'IM' endian mark
    int32_t *h = (int32_t *)&header[128];

    int mask_length = count_mask_bits(filter_mask);
    
    /* Write out the index array tying data back to original BPM ids. */
    int padding = write_matrix_header(&h,
        mxDOUBLE_CLASS, "ids", miUINT8, mask_length, 2, 1, mask_length);
    compute_mask_ids((uint8_t *)h, filter_mask);
    h = (int32_t *)((char *)h + mask_length + padding);

    /* Finally write out the matrix header for the fa data. */
    write_matrix_header(&h,
        matlab_double ? mxDOUBLE_CLASS : mxINT32_CLASS,
        "fa", miINT32, dump_length * mask_length * 8,
        3, 2, mask_length, dump_length);
    
    ASSERT_write(file_out, header, (char *) h - header);
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
        int backlog_frames = header.h.write_backlog / FA_FRAME_SIZE;
        if (backlog_frames > backoff_threshold)
        {
            if (show_progress)
                fprintf(stderr, "Backlog %d, pausing for 1 second\r",
                    backlog_frames);
            sleep(1);
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
                file_fd = open(archive_file_name, O_RDONLY),
                "Unable to open file \"%s\"", archive_file_name)  &&
            prepare_header()  &&
            IF_(out_file_name != NULL,
                TEST_IO_(
                    file_out = open(
                        out_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0666),
                    "Unable to open output file \"%s\"", out_file_name));

        if (ok)
        {
            if (show_header)
            {
                FILE *out;
                ASSERT_NULL(out = fdopen(file_out, "w"));
                print_header(out, &header);
            }
            else
            {
                if (matlab_format)
                    write_matlab_header();
                dump_data();
            }
        }
        
        return ok ? 0 : 2;
    }
    else
        return 1;
}
