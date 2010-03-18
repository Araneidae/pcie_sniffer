/* Common routines for disk access. */

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


void initialise_header(struct disk_header *header, uint64_t data_size)
{
    memset(header, 0, sizeof(*header));
    strncpy(header->h.signature, DISK_SIGNATURE, sizeof(header->h.signature));
    header->h.version = 1;
    header->h.data_start = DISK_HEADER_SIZE;
    header->h.data_size = data_size;
    header->h.block_count = 0;
    header->h.max_block_count = MAX_HEADER_BLOCKS;
}


bool validate_header(struct disk_header *header, off64_t file_size)
{
    off64_t data_start = header->h.data_start;
    off64_t data_size = header->h.data_size;
    bool ok =
        TEST_OK_(strncmp(header->h.signature, DISK_SIGNATURE,
            sizeof(header->h.signature)) == 0, "Invalid header signature")  &&
        TEST_OK_(header->h.version == 1,
            "Invalid header version %u", header->h.version)  &&
        TEST_OK_(data_start == DISK_HEADER_SIZE,
            "Unexpected data start: %llu", data_start)  &&
        TEST_OK_(data_size + data_start == file_size,
            "Invalid data size %llu in header", data_size)  &&
        TEST_OK_(data_size % DATA_LOCK_BLOCK_SIZE == 0,
            "Uneven size data area")  &&
        TEST_OK_(header->h.max_block_count == MAX_HEADER_BLOCKS,
            "Unexpected header block count: %u", header->h.max_block_count)  &&
        TEST_OK_(header->h.block_count <= header->h.max_block_count,
            "Too many blocks: %u", header->h.block_count);
    
    /* Validate the blocks: there should be no overlap in offsets.  We don't
     * check the timestamps, as these are advisory only. */
    off64_t data_stop = header->blocks[0].stop_offset;
    off64_t last_start = data_stop;
    bool wrapped = false;
    for (uint32_t i = 0; ok  &&  i < header->h.block_count; i ++)
    {
        off64_t start = header->blocks[i].start_offset;
        off64_t stop  = header->blocks[i].stop_offset;

        ok =
            TEST_OK_(stop == last_start,
                "Block %d doesn't follow previous block", i)  &&
            TEST_OK_(start < data_size, "Block %d starts outside file", i)  &&
            TEST_OK_(stop < data_size, "Block %d stop outside file", i);
        last_start = start;

        /* Check when data wraps around end of buffer.  This can only happen
         * once, and once we're wrapped we have to check for wrapping past
         * the write point. */
        if (ok && stop < start)
        {
            ok = TEST_OK_(!wrapped, "Block %d inconsistent length", i);
            wrapped = true;
        }
        if (ok && wrapped)
            ok = TEST_OK_(start >= data_stop,
                "Block %d wraps past start of data area", i);
    }

    return ok;
}


void print_header(FILE *out, struct disk_header *header)
{
    fprintf(out,
        "FA sniffer archive: %.7s, v%d.  Data size = %llu, offset %llu\n"
        "Blocks: %d of %d\n",
        header->h.signature, header->h.version,
        header->h.data_size, header->h.data_start,
        header->h.block_count, header->h.max_block_count);
    for (uint32_t i = 0; i < header->h.block_count; i ++)
    {
        struct block_record *block = &header->blocks[i];
        fprintf(out, " %d: %lld-%lld (%lld-%lld)\n", i,
            block->start_offset, block->stop_offset,
            block->start_sec, block->stop_sec);
    }
}


bool get_filesize(int disk_fd, uint64_t *file_size)
{
    struct stat stat;
    return
        TEST_IO(fstat(disk_fd, &stat))  &&
        DO_(*file_size = stat.st_size);
}


bool read_header(int disk_fd, struct disk_header *header)
{
    struct flock flock = {
        .l_type = F_RDLCK, .l_whence = SEEK_SET,
        .l_start = 0, .l_len = sizeof(*header)
    };

    return
        TEST_IO(fcntl(disk_fd, F_SETLKW, &flock))  &&
        TEST_IO(lseek(disk_fd, 0, SEEK_SET))  &&
        
        TEST_OK(read(disk_fd, header, sizeof(*header)) == sizeof(*header))  &&
        DO_(flock.l_type = F_UNLCK)  &&
        TEST_IO(fcntl(disk_fd, F_SETLK, &flock));
}


bool write_header(int disk_fd, struct disk_header *header)
{
    struct flock flock = {
        .l_type = F_WRLCK, .l_whence = SEEK_SET,
        .l_start = 0, .l_len = sizeof(*header)
    };

    return
        TEST_IO(fcntl(disk_fd, F_SETLKW, &flock))  &&
        TEST_IO(lseek(disk_fd, 0, SEEK_SET))  &&
        
        TEST_OK(write(disk_fd, header, sizeof(*header)) == sizeof(*header))  &&
        DO_(flock.l_type = F_UNLCK)  &&
        TEST_IO(fcntl(disk_fd, F_SETLK, &flock));
}


void dump_binary(FILE *out, void *buffer, size_t length)
{
    uint8_t *dump = buffer;

    for (size_t a = 0; a < length; a += 16)
    {
        printf("%08x: ", a);
        for (int i = 0; i < 16; i ++)
        {
            if (a + i < length)
                printf(" %02x", dump[a+i]);
            else
                printf("   ");
            if (i % 16 == 7)
                printf(" ");
        }

        printf("  ");
        for (int i = 0; i < 16; i ++)
        {
            uint8_t c = dump[a+i];
            if (a + i < length)
                printf("%c", 32 <= c  &&  c < 127 ? c : '.');
            else
                printf(" ");
            if (i % 16 == 7)
                printf(" ");
        }
        printf("\n");
    }
    if (length % 16 != 0)
        printf("\n");
}
