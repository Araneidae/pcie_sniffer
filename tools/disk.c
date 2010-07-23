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
#include <sys/ioctl.h>
#include <linux/fs.h>

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
    header->h.write_backlog = 0;
    header->h.disk_status = 0;
    header->h.write_buffer = 0;
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
        TEST_OK_(data_size + data_start <= file_size,
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


static const char * status_strings[] = {
    "Inactive",
    "Writing",
    "No data available"
};

void print_header(FILE *out, struct disk_header *header)
{
    int64_t data_size = header->h.data_size / FA_FRAME_SIZE;
    const char * status =
        header->h.disk_status < ARRAY_SIZE(status_strings) ?
        status_strings[header->h.disk_status] : "Unknown";
    fprintf(out,
        "FA sniffer archive: %.7s, v%d.\n"
        "Data size = %llu frames (%llu bytes), offset %llu bytes\n"
        "Status: %s, write backlog: %d (%.2f%%), buffer %d bytes\n"
        "Blocks: %d of %d\n",
        header->h.signature, header->h.version,
        data_size, header->h.data_size,
        header->h.data_start,
        status, header->h.write_backlog / FA_FRAME_SIZE,
        100.0 * header->h.write_backlog / header->h.write_buffer,
        header->h.write_buffer,
        header->h.block_count, header->h.max_block_count);
    for (uint32_t i = 0; i < header->h.block_count; i ++)
    {
        struct block_record *block = &header->blocks[i];
        int64_t start = block->start_offset / FA_FRAME_SIZE;
        int64_t stop  = block->stop_offset  / FA_FRAME_SIZE;
        int64_t length = stop - start;
        if (length <= 0)
            length += data_size;
        fprintf(out, "%3d: %lld-%lld (%lld frames)\n", i, start, stop, length);
    }
}


bool get_filesize(int disk_fd, uint64_t *file_size)
{
    /* First try blocksize, if that fails try stat: the first works on a
     * block device, the second on a regular file. */
    if (ioctl(disk_fd, BLKGETSIZE64, file_size) == 0)
        return true;
    else
    {
        struct stat st;
        return
            TEST_IO(fstat(disk_fd, &st))  &&
            DO_(*file_size = st.st_size)  &&
            TEST_OK_(*file_size > 0,
                "Zero file size.  Maybe stat failed?");
    }
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
