/* Utility to prepare a file for use as an archive area by the FA sniffer
 * archiver application. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "error.h"
#include "disk.h"


#define PROGRESS_INTERVAL   (1 << 8)

/* An experiment shows that a disk block transfer size of 512K is optimal in the
 * sense of being the largest single block transfer size. */
#define K               1024
#define FA_BLOCK_SIZE   (512 * K)    // Default block size for device IO


/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/

static char *argv0;

static const char * file_name;
static uint64_t file_size;
static bool file_size_given = false;
static uint64_t block_size = FA_BLOCK_SIZE;


static void usage(void)
{
    printf(
"Usage: %s [<options>] <file-name>\n"
"\n"
"Prepares or reinitalises a disk file <file-name> for use as an FA sniffer\n"
"archive.  The given <file-name> can be a block device or an ordinary file.\n"
"\n"
"The following options can be given:\n"
"   -s:  Specify size of file.  The file will be resized to the given size\n"
"        and filled with zeros.\n"
"   -b:  Specify block size for IO transfers to disk.  This should match\n"
"        the disk's IO block size.  The default value is %lld.\n"
"\n"
"Both block size and file size can be followed by one of K, M, G or T to\n"
"specify sizes in kilo, mega, giga or terabytes.\n"
        , argv0, block_size);
}


static bool read_size(const char *string, uint64_t *size)
{
    char *end;
    *size = strtoll(string, &end, 0);
    if (!TEST_OK_(end > string, "Size \"%s\" is not a number", string))
        return false;
    switch (*end)
    {
        case 'K':   *size <<= 10;  end ++;  break;
        case 'M':   *size <<= 20;  end ++;  break;
        case 'G':   *size <<= 30;  end ++;  break;
        case 'T':   *size <<= 40;  end ++;  break;
    }
    return TEST_OK_(*end == '\0', "Malformed size \"%s\"", string);
}


static bool process_opts(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hs:b:"))
        {
            case 'h':
                usage();
                exit(0);
            case 's':
                ok = read_size(optarg, &file_size);
                file_size_given = true;
                break;
            case 'b':
                ok = read_size(optarg, &block_size);
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
        TEST_OK_(argc == 1, "Try -h for usage")  &&
        TEST_OK_((uint32_t) block_size == block_size,
            "Unfeasibly large block size")  &&
        TEST_OK_(block_size % sysconf(_SC_PAGESIZE) == 0,
            "Block size must be a multiple of page size")  &&
        DO_(file_name = *argv);
}


/*****************************************************************************/
/*                                                                           */
/*****************************************************************************/


static bool write_new_header(int file_fd, uint64_t data_size)
{
    struct disk_header *header;
    bool ok = TEST_NULL(header = valloc(DISK_HEADER_SIZE));
    if (ok)
    {
        initialise_header(header, block_size, data_size);
        ok =
            validate_header(header, data_size + DISK_HEADER_SIZE)  &&
            TEST_OK(
                write(file_fd, header, DISK_HEADER_SIZE) == DISK_HEADER_SIZE);
        print_header(stdout, header);
        free(header);
    }
    return ok;
}


static void show_progress(uint64_t n, uint64_t final_n)
{
    const char *progress = "|/-\\";
    if (n % PROGRESS_INTERVAL == 0)
    {
        printf("%c %9llu (%5.2f%%)\r",
            progress[(n / PROGRESS_INTERVAL) % 4], n,
            100.0 * (double) n / final_n);
        fflush(stdout);
    }
}


static bool fill_zeros(int file_fd, uint64_t data_size)
{
    void *zeros = valloc(block_size);
    if (!TEST_NULL(zeros))
        return false;
    memset(zeros, 0, block_size);

    uint64_t final_n = data_size / block_size;
    bool ok = true;
    for (uint64_t n = 0; ok  &&  data_size >= block_size;
         data_size -= block_size, n += 1)
    {
        ok = TEST_write(file_fd, zeros, block_size);
        show_progress(n, final_n);
    }
    printf("\n");
    return ok;
}


/* Computes a data size compatible with the file size. */

static bool compute_data_size(int file_fd, uint64_t *data_size)
{
    uint64_t block_count = (file_size - DISK_HEADER_SIZE) / block_size;
    *data_size = block_count * block_size;
    return
        TEST_OK_(file_size > DISK_HEADER_SIZE  &&  *data_size > 0,
            "File size %llu too small", file_size);
}


int main(int argc, char **argv)
{
    if (!process_args(argc, argv))
        /* For argument errors return 1. */
        return 1;

    int file_fd;
    int open_flags = file_size_given ? O_CREAT | O_TRUNC : 0;
    uint64_t data_size;
    bool ok =
        TEST_IO_(file_fd = open(file_name,
            O_WRONLY | O_DIRECT | open_flags, 0664),
            "Unable to write to file \"%s\"", file_name)  &&
        IF_(!file_size_given, get_filesize(file_fd, &file_size))  &&
        compute_data_size(file_fd, &data_size)  &&
        write_new_header(file_fd, data_size)  &&
        IF_(file_size_given, fill_zeros(file_fd, data_size))  &&
        TEST_IO(close(file_fd));

    return ok ? 0 : 2;
}
