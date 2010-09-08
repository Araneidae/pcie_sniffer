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


static void usage(void)
{
    printf(
"Usage: %s [<options>] <capture-mask> <file-name>\n"
"\n"
"Prepares or reinitalises a disk file <file-name> for use as an FA sniffer\n"
"archive.  The given <file-name> can be a block device or an ordinary file.\n"
"The BPMs specified in <capture-mask> will be captured to disk.\n"
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
"\n"
"File size can be followed by one of K, M, G or T to specify sizes in\n"
"kilo, mega, giga or terabytes, and similarly block sizes can be followed\n"
"by one of K or M.\n"
        , argv0,
        input_block_size, output_block_size,
        first_decimation, second_decimation);
}


static bool process_opts(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hs:I:O:d:D:"))
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


static bool write_new_header(int file_fd)
{
    struct disk_header *header;
    return
        TEST_NULL(header = valloc(DISK_HEADER_SIZE))  &&
        FINALLY(
            initialise_header(header,
                archive_mask, file_size,
                input_block_size, output_block_size,
                first_decimation, second_decimation)  &&
            TEST_IO(lseek(file_fd, 0, SEEK_SET))  &&
            TEST_write(file_fd, header, DISK_HEADER_SIZE)  &&
            DO_(print_header(stdout, header)),

            // Finally always do this:
            DO_(free(header)));
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


static bool fill_zeros(int file_fd)
{
    uint32_t block_size = 512*K;
    void *zeros = valloc(block_size);
    memset(zeros, 0, block_size);

    int final_n = file_size / block_size;
    uint64_t size_left = file_size;
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

    int file_fd;
    int open_flags = file_size_given ? O_CREAT | O_TRUNC : 0;
    bool ok =
        TEST_IO_(file_fd = open(file_name,
            O_WRONLY | O_DIRECT | open_flags, 0664),
            "Unable to write to file \"%s\"", file_name)  &&
        IF_ELSE(file_size_given,
            fill_zeros(file_fd),
            get_filesize(file_fd, &file_size))  &&
        write_new_header(file_fd)  &&
        TEST_IO(close(file_fd));

    return ok ? 0 : 2;
}
