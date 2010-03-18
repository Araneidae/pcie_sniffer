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


#define USAGE_STRING \
    "Usage: prepare <file-name> <disk-size>\n" \
    "\n" \
    "Prepares disk file <file-name> for use as FA sniffer archive file by\n" \
    "writing a suitable header block and filling <disk-size> bytes of\n" \
    "data area with zeros."

bool read_disk_size(const char *string, uint64_t *size)
{
    char *end;
    *size = strtoll(string, &end, 0);
    if (!TEST_OK_(end > string, "Disk size \"%s\" is not a number!", string))
        return false;
    switch (*end)
    {
        case 'M':   *size <<= 20;  end ++;  break;
        case 'G':   *size <<= 30;  end ++;  break;
        case 'T':   *size <<= 40;  end ++;  break;
    }
    return TEST_OK_(*end == '\0', "Malformed disk size \"%s\"", string);
}


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
        ok = TEST_OK(
            write(file_fd, zeros, ZEROS_BLOCK_SIZE) == ZEROS_BLOCK_SIZE);
        show_progress(n, final_n);
    }
    if (ok && size > 0)
        ok = TEST_OK(write(file_fd, zeros, size) == (ssize_t) size);
    printf("\n");
    return ok;
}


int main(int argc, char **argv)
{
    uint64_t data_size;
    bool ok =
        TEST_OK_(argc == 3, USAGE_STRING)  &&
        read_disk_size(argv[2], &data_size)  &&
        TEST_OK_(data_size % DATA_LOCK_BLOCK_SIZE == 0,
            "Data size %llu must be multiple of %d",
            data_size, DATA_LOCK_BLOCK_SIZE);
    if (!ok)
        /* For argument errors return 1. */
        return 1;

    const char *file_name = argv[1];
    int file_fd;
    ok =
        TEST_IO(file_fd = open(file_name,
            O_WRONLY | O_DIRECT | O_CREAT | O_TRUNC, 0664))  &&
        write_new_header(file_fd, data_size)  &&
        fill_zeros(file_fd, data_size)  &&
        TEST_IO(close(file_fd));
    
    return ok ? 0 : 2;
}
