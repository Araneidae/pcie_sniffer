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


#define ZEROS_BLOCK_SIZE    (1 << 16)
#define PROGRESS_INTERVAL   (1 << 8)


/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/

static char *argv0;

uint64_t data_size;
char * file_name;
bool header_only = false;


void usage(void)
{
    printf(
"Usage: %s [options] <file-name> <disk-size>\n"
"\n"
"Prepares disk file <file-name> for use as FA sniffer archive file by\n"
"writing a suitable header block and filling <disk-size> bytes of\n"
"data area with zeros.\n"
"\n"
"Options:\n"
"   -H  Only initialise the header, take existing file size.\n"
        , argv0);
}

bool read_disk_size(const char *string, uint64_t *size)
{
    char *end;
    *size = strtoll(string, &end, 0);
    if (!TEST_OK_(end > string, "Disk size \"%s\" is not a number", string))
        return false;
    switch (*end)
    {
        case 'M':   *size <<= 20;  end ++;  break;
        case 'G':   *size <<= 30;  end ++;  break;
        case 'T':   *size <<= 40;  end ++;  break;
    }
    return TEST_OK_(*end == '\0', "Malformed disk size \"%s\"", string);
}


bool process_opts(int *argc, char ***argv)
{
    argv0 = (*argv)[0];
    bool ok = true;
    while (ok)
    {
        switch (getopt(*argc, *argv, "+hH"))
        {
            case 'h':   usage();                                    exit(0);
            case 'H':   header_only = true;                         break;
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


bool process_args(int argc, char **argv)
{
#define PROCESS_ARG(missing, action) \
    ( TEST_OK_(argc >= 1, missing " argument missing")  && \
      action  && \
      DO_(argc -= 1; argv += 1) )
    
    return
        PROCESS_ARG("Filename",
            DO_(file_name = *argv))  &&
        IF_(!header_only,
            PROCESS_ARG("File size",
                read_disk_size(*argv, &data_size)  &&
                TEST_OK_(data_size % DATA_LOCK_BLOCK_SIZE == 0,
                    "Data size %llu must be multiple of %d",
                    data_size, DATA_LOCK_BLOCK_SIZE)))  &&
        TEST_OK_(argc == 0, "Too many arguments");
#undef PROCESS_ARG
}


/*****************************************************************************/
/*                                                                           */
/*****************************************************************************/


bool write_new_header(int file_fd, uint64_t data_size)
{
    struct disk_header *header;
    bool ok = TEST_NULL(header = valloc(DISK_HEADER_SIZE));
    if (ok)
    {
        initialise_header(header, data_size);
        ok =
            validate_header(header, data_size + DISK_HEADER_SIZE)  &&
            TEST_OK(
                write(file_fd, header, DISK_HEADER_SIZE) == DISK_HEADER_SIZE);
        free(header);
    }
    return ok;
}


void show_progress(uint64_t n, uint64_t final_n)
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


bool fill_zeros(int file_fd, uint64_t size)
{
    void *zeros = valloc(ZEROS_BLOCK_SIZE);
    if (!TEST_NULL(zeros))
        return false;
    memset(zeros, 0, ZEROS_BLOCK_SIZE);

    uint64_t final_n = size / ZEROS_BLOCK_SIZE;
    bool ok = true;
    for (uint64_t n = 0; ok  &&  size >= ZEROS_BLOCK_SIZE;
         size -= ZEROS_BLOCK_SIZE, n += 1)
    {
        ok = TEST_write(file_fd, zeros, ZEROS_BLOCK_SIZE);
        show_progress(n, final_n);
    }
    if (ok && size > 0)
        ok = TEST_write(file_fd, zeros, size);
    printf("\n");
    return ok;
}


/* Computes a data size compatible with the given file size. */

bool compute_data_size(int file_fd, uint64_t *data_size)
{
    uint64_t file_size;
    return
        get_filesize(file_fd, &file_size)  &&
        DO_(*data_size =
            (file_size - DISK_HEADER_SIZE) & ~(DATA_LOCK_BLOCK_SIZE - 1))  &&
        TEST_OK_(file_size > DISK_HEADER_SIZE  &&  *data_size > 0,
            "File size %llu too small", file_size);
}


int main(int argc, char **argv)
{
    bool ok =
        process_opts(&argc, &argv)  &&
        process_args(argc, argv);
    if (!ok)
        /* For argument errors return 1. */
        return 1;

    int file_fd;
    int open_flags = header_only ? 0 : O_CREAT | O_TRUNC;
    ok =
        TEST_IO(file_fd = open(file_name,
            O_WRONLY | O_DIRECT | open_flags, 0664))  &&
        IF_(header_only, compute_data_size(file_fd, &data_size))  &&
        write_new_header(file_fd, data_size)  &&
        IF_(!header_only, fill_zeros(file_fd, data_size))  &&
        TEST_IO(close(file_fd));
    
    return ok ? 0 : 2;
}
