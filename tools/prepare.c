/* Utility to prepare a file for use as an archive area by the FA sniffer
 * archiver application. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "transform.h"
#include "disk.h"
#include "parse.h"



/* An experiment shows that a disk block transfer size of 512K is optimal in the
 * sense of being the largest single block transfer size. */
#define K               1024
#define FA_BLOCK_SIZE   (512 * K)    // Default block size for device IO


/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/

static char *argv0;

static const char * file_name;
static bool file_size_given = false;
static uint64_t file_size;
static filter_mask_t archive_mask;
static uint32_t input_block_size = 512 * K;
static uint32_t output_block_size = 512 * K;
static uint32_t first_decimation = 64;
static uint32_t second_decimation = 256;
static double sample_frequency = 10072.4;

static bool read_only = false;


static void usage(void)
{
    printf(
"Usage: %s [<options>] <capture-mask> <file-name>\n"
"or:    %s -H <file-name>\n"
"\n"
"Prepares or reinitalises a disk file <file-name> for use as an FA sniffer\n"
"archive unless -H is given.  The given <file-name> can be a block device or\n"
"an ordinary file.  The BPMs specified in <capture-mask> will be captured to\n"
"disk.\n"
"\n"
"The following options can be given:\n"
"   -s:  Specify size of file.  The file will be resized to the given size\n"
"        and filled with zeros.\n"
"   -I:  Specify input block size for reads from FA sniffer device.  The\n"
"        default value is %"PRIu32" bytes.\n"
"   -O:  Specify block size for IO transfers to disk.  This should match\n"
"        the disk's IO block size.  The default value is %"PRIu32".\n"
"   -d:  Specify first decimation factor.  The default value is %"PRIu32".\n"
"   -D:  Specify second decimation factor.  The default value is %"PRIu32".\n"
"   -f:  Specify nominal sample frequency.  The default is %.1gHz\n"
"\n"
"File size can be followed by one of K, M, G or T to specify sizes in\n"
"kilo, mega, giga or terabytes, and similarly block sizes can be followed\n"
"by one of K or M.\n"
"\n"
"If instead -H is given then the file header will be printed.\n"
        , argv0, argv0,
        input_block_size, output_block_size,
        first_decimation, second_decimation,
        sample_frequency);
}


static bool process_opts(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hs:I:O:d:D:f:"))
        {
            case 'h':
                usage();
                exit(0);
            case 's':
                ok = DO_PARSE("file size", parse_size64, optarg, &file_size);
                file_size_given = true;
                break;
            case 'I':
                ok = DO_PARSE("input block size",
                    parse_size32, optarg, &input_block_size);
                break;
            case 'O':
                ok = DO_PARSE("output block size",
                    parse_size32, optarg, &output_block_size);
                break;
            case 'd':
                ok = DO_PARSE("first decimation",
                    parse_size32, optarg, &first_decimation);
                break;
            case 'D':
                ok = DO_PARSE("second decimation",
                    parse_size32, optarg, &second_decimation);
                break;
            case 'f':
                ok = DO_PARSE("sample frequency",
                    parse_double, optarg, &sample_frequency);
                break;
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


static bool process_args(int argc, char **argv)
{
    if (argc >= 2  &&  strcmp(argv[1], "-H") == 0)
    {
        read_only = true;
        return
            TEST_OK_(argc == 3, "Try -h for usage")  &&
            DO_(file_name = argv[2]);
    }
    else
        return
            process_opts(&argc, &argv)  &&
            TEST_OK_(argc == 2, "Try -h for usage")  &&
            DO_PARSE("capture mask", parse_mask, argv[0], archive_mask)  &&
            DO_(file_name = argv[1]);
}


/*****************************************************************************/
/*                                                                           */
/*****************************************************************************/

#define PROGRESS_INTERVAL   16


/* Resets the index area to zeros, even if the rest of the file is untouched.
 * This is necessary for a consistent freshly initialised database. */
static bool reset_index(int file_fd, int index_data_size)
{
    void *data_index;
    bool ok =
        TEST_NULL(data_index = valloc(index_data_size))  &&
        DO_(memset(data_index, 0, index_data_size))  &&
        TEST_write(file_fd, data_index, index_data_size);
    free(data_index);
    return ok;
}

static bool write_new_header(int file_fd, int *written)
{
    struct disk_header *header;
    bool ok =
        TEST_NULL(header = valloc(DISK_HEADER_SIZE))  &&
        initialise_header(header,
            archive_mask, file_size,
            input_block_size, output_block_size,
            first_decimation, second_decimation, sample_frequency)  &&
        TEST_IO(lseek(file_fd, 0, SEEK_SET))  &&
        TEST_write(file_fd, header, DISK_HEADER_SIZE)  &&
        reset_index(file_fd, header->index_data_size)  &&
        DO_(print_header(stdout, header))  &&
        DO_(*written = DISK_HEADER_SIZE + header->index_data_size);
    free(header);
    return ok;
}


static void show_progress(int n, int final_n)
{
    const char *progress = "|/-\\";
    if (n % PROGRESS_INTERVAL == 0)
    {
        printf("%c %9d (%5.2f%%)\r",
            progress[(n / PROGRESS_INTERVAL) % 4], n,
            100.0 * (double) n / final_n);
        fflush(stdout);
    }
}


static bool fill_zeros(int file_fd, int written)
{
    uint32_t block_size = 512*K;
    void *zeros = valloc(block_size);
    memset(zeros, 0, block_size);

    uint64_t size_left = file_size - written;
    int final_n = size_left / block_size;
    bool ok = true;
    for (int n = 0; ok  &&  size_left >= block_size;
         size_left -= block_size, n += 1)
    {
        ok = TEST_write(file_fd, zeros, block_size);
        show_progress(n, final_n);
    }
    if (ok  &&  size_left > 0)
        ok = TEST_write(file_fd, zeros, size_left);
    printf("\n");
    return ok;
}


int main(int argc, char **argv)
{
    if (!process_args(argc, argv))
        /* For argument errors return 1. */
        return 1;

    bool ok;
    int file_fd;
    if (read_only)
    {
        char header[DISK_HEADER_SIZE];
        ok =
            TEST_IO_(file_fd = open(file_name, O_RDONLY),
                "Unable to read file \"%s\"", file_name)  &&
            TEST_read(file_fd, header, DISK_HEADER_SIZE)  &&
            DO_(print_header(stdout, (struct disk_header *) header));
    }
    else
    {
        int open_flags = file_size_given ? O_CREAT | O_TRUNC : 0;
        int written;
        ok =
            TEST_IO_(file_fd = open(file_name,
                O_WRONLY | O_DIRECT | open_flags, 0664),
                "Unable to write to file \"%s\"", file_name)  &&
            lock_archive(file_fd)  &&
            IF_(!file_size_given,
                get_filesize(file_fd, &file_size))  &&
            write_new_header(file_fd, &written)  &&
            IF_(file_size_given,
                fill_zeros(file_fd, written))  &&
            TEST_IO(close(file_fd));
    }

    return ok ? 0 : 2;
}
