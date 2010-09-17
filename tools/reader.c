/* Implements reading from disk. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "parse.h"
#include "transform.h"
#include "locking.h"
#include "disk.h"
#include "disk_writer.h"
#include "socket_server.h"

#include "reader.h"


/* Each connection opens its own file handle on the archive.  This is the
 * archive file. */
static const char *archive_filename;



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer pool. */

DECLARE_LOCKING(buffer_lock);

struct pool_entry {
    struct pool_entry *next;
    struct fa_entry buffer[];
};

static int pool_size;
static struct pool_entry *buffer_pool = NULL;


static bool lock_fa_buffers(struct fa_entry ***buffers, int count)
{
    bool ok;
    LOCK(buffer_lock);
    ok = TEST_OK_(count <= pool_size, "Read too busy");
    if (ok)
    {
        pool_size -= count;
        *buffers = malloc(count * sizeof(struct fa_entry *));
        for (int i = 0; i < count; i ++)
        {
            struct pool_entry *entry = buffer_pool;
            buffer_pool = entry->next;
            (*buffers)[i] = entry->buffer;
        }
    }
    UNLOCK(buffer_lock);
    return ok;
}

static void unlock_fa_buffers(struct fa_entry **buffers, int count)
{
    LOCK(buffer_lock);
    for (int i = 0; i < count; i ++)
    {
        struct pool_entry *entry = (struct pool_entry *) (
            (char *) buffers[i] - offsetof(struct pool_entry, buffer));
        entry->next = buffer_pool;
        buffer_pool = entry;
    }
    pool_size += count;
    UNLOCK(buffer_lock);
    free(buffers);
}

static bool initialise_buffer_pool(int count)
{
    int buffer_size = FA_ENTRY_SIZE * get_header()->major_sample_count;

    for (int i = 0; i < count; i ++)
    {
        struct pool_entry *entry =
            malloc(sizeof(struct pool_entry) + buffer_size);
        entry->next = buffer_pool;
        buffer_pool = entry;
    }
    pool_size = count;

    return true;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reading from disk. */

static bool read_fa_block(int archive, int major_block, int id, void *block)
{
    const struct disk_header *header = get_header();
    int fa_block_size = FA_ENTRY_SIZE * header->major_sample_count;
    off64_t offset =
        header->major_data_start +
        (uint64_t) header->major_block_size * major_block +
        fa_block_size * id;
    return
        DO_(request_read())  &&
        TEST_IO(lseek(archive, offset, SEEK_SET))  &&
        TEST_read(archive, block, fa_block_size);
}


struct iter_mask {
    int count;
    uint16_t index[FA_ENTRY_COUNT];
};

/* Converts an external mask into indexes into the archive. */
static bool mask_to_archive(const filter_mask_t mask, struct iter_mask *iter)
{
    const struct disk_header *header = get_header();
    int ix = 0;
    int n = 0;
    bool ok = true;
    for (int i = 0; ok  &&  i < FA_ENTRY_COUNT; i ++)
    {
        if (test_mask_bit(mask, i))
        {
            ok = TEST_OK_(test_mask_bit(header->archive_mask, i),
                "BPM %d not in archive", i);
            iter->index[n] = ix;
            n += 1;
        }
        if (test_mask_bit(header->archive_mask, i))
            ix += 1;
    }
    iter->count = n;
    return ok;
}


static void read_fa_data(
    int scon, struct iter_mask *iter,
    unsigned int block, unsigned int offset, int count)
{
    struct fa_entry **read_buffers = NULL;
    int archive = -1;
    bool ok =
        lock_fa_buffers(&read_buffers, iter->count)  &&
        TEST_IO(archive = open(archive_filename, O_RDONLY));
    report_socket_error(scon, ok);

    /* Bytes in a single frame to be written. */
    int line_size = iter->count * sizeof(struct fa_entry);
    struct fa_entry write_buffer[FA_ENTRY_COUNT * 32];
    const struct disk_header *header = get_header();

    while (ok  &&  count > 0)
    {
        /* Read the FA data directly from the archive. */
        for (int i = 0; ok  &&  i < iter->count; i ++)
            ok = read_fa_block(archive, block, i, read_buffers[i]);

        /* Transpose the read data into sensible format and write out in
         * sensible sized chunks. */
        while (ok  &&  offset < header->major_sample_count  &&  count > 0)
        {
            struct fa_entry *p = write_buffer;
            size_t buf_size = 0;
            while (count > 0  &&
                offset < header->major_sample_count  &&
                buf_size + line_size <= sizeof(write_buffer))
            {
                for (int i = 0; i < iter->count; i ++)
                    *p++ = read_buffers[i][offset];

                count -= 1;
                offset += 1;
                buf_size += line_size;
            }
            ok = TEST_write(scon, write_buffer, buf_size);
        }

        block = (block + 1) % header->major_block_count;
        offset = 0;
    }

    if (read_buffers != NULL)
        unlock_fa_buffers(read_buffers, iter->count);
    if (archive != -1)
        close(archive);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read request parsing. */

/* A read request can be quite complicated, though the syntax is quite
 * constrained.  The options are:
 *
 *  Data source being read.  Options here are:
 *      Normal FA data
 *      Decimated data, either mean only or complete samples
 *      Double decimated data, ditto
 *  Mask of BPM ids to read.
 *  Data start point.  This can be specifed as:
 *      Timestamp
 *
 * Very simple syntax (no spaces allowed):
 *
 *  read-request = "R" source mask start "N" samples
 *  start = "T" date-time | "S" seconds [ "." nanoseconds ]
 *  source = "F" | "D" ["D"] ["M"]
 *  samples = integer
 *
 * */

enum data_source {
    READ_FA,
    READ_D_ALL,
    READ_D_MEAN,
    READ_DD_ALL,
    READ_DD_MEAN,
};


/* Result of parsing a read command. */
struct read_parse {
    filter_mask_t read_mask;        // List of BPMs to be read
    unsigned int samples;                    // Requested number of samples
    uint64_t start;
    enum data_source data_source;
};


static bool parse_source(const char **string, enum data_source *source)
{
    if (read_char(string, 'F'))
        *source = READ_FA;
    else if (read_char(string, 'D'))
    {
        bool dd = read_char(string, 'D');
        bool mean = read_char(string, 'M');
        if (dd)
            *source = mean ? READ_DD_MEAN : READ_DD_ALL;
        else
            *source = mean ? READ_D_MEAN : READ_D_ALL;
    }
    else
        return TEST_OK_(false, "Invalid source specification");
    return true;
}

static bool parse_start(const char **string, uint64_t *start)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
    bool ok;
    if (read_char(string, 'T'))
        ok = parse_datetime(string, &ts);
    else if (read_char(string, 'S'))
        ok = parse_seconds(string, &ts);
    else
        ok = TEST_OK_(false, "Expected T or S for timestamp");
    ok = ok  &&  TEST_OK_(ts.tv_sec > 0, "Timestamp ridiculously early");
    *start = 1000000 * (uint64_t) ts.tv_sec + ts.tv_nsec / 1000;
    return ok;
}

static bool parse_read_request(const char **string, struct read_parse *parse)
{
    return
        parse_char(string, 'R')  &&
        parse_source(string, &parse->data_source)  &&
        parse_mask(string, parse->read_mask)  &&
        parse_start(string, &parse->start)  &&
        parse_char(string, 'N')  &&
        parse_uint(string, &parse->samples);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read processing. */


/* Convert timestamp into block index. */
void process_read(int scon, const char *buf)
{
    struct read_parse parse;
    unsigned int block, offset;
    struct iter_mask iter;

    push_error_handling();      // Popped by report_socket_error()
    bool ok =
        DO_PARSE("read request", parse_read_request, buf, &parse)  &&
        timestamp_to_index(parse.start, parse.samples, &block, &offset)  &&
        mask_to_archive(parse.read_mask, &iter)  &&
        TEST_OK_(parse.data_source == READ_FA, "Only FA data supported");

    if (ok)
        read_fa_data(scon, &iter, block, offset, parse.samples);
    else
        report_socket_error(scon, ok);
}


bool initialise_reader(const char *archive)
{
    archive_filename = archive;
    return initialise_buffer_pool(256);
}
