/* Implements reading from disk. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
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

#define K   1024


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

typedef struct fa_entry **read_buffers_t;

static bool lock_buffers(read_buffers_t *buffers, int count)
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

static void unlock_buffers(read_buffers_t buffers, int count)
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
    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // This needs to be revisited, should be a more general unified calculation,
    // probably derived from a disk_block_size parameter or similar.
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
/* Reading from disk: general support. */


struct iter_mask {
    unsigned int count;
    uint16_t index[FA_ENTRY_COUNT];
};


/* Converts an external mask into indexes into the archive. */
static bool mask_to_archive(const filter_mask_t mask, struct iter_mask *iter)
{
    const struct disk_header *header = get_header();
    unsigned int ix = 0;
    unsigned int n = 0;
    bool ok = true;
    for (unsigned int i = 0; ok  &&  i < FA_ENTRY_COUNT; i ++)
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


struct reader {
    /* Converts timestamp into block index and offset into block, suitable for
     * passing to read_block() function, also checks that the requested number
     * of samples are available in the archive. */
    bool (*timestamp_to_index)(
        uint64_t start, unsigned int samples,
        unsigned int *block, unsigned int *offset);
    /* Reads the requested block from archive into buffer. */
    bool (*read_block)(
        int archive, unsigned int block, unsigned int i,
        void *buffer, unsigned int *samples);
    /* Writes a single line from a list of buffers to an output buffer. */
    void (*write_line)(
        unsigned int count, read_buffers_t read_buffers, unsigned int offset,
        unsigned int data_mask, char *output);
    /* The size of a single output value. */
    unsigned int (*output_size)(unsigned int data_mask);

    unsigned int block_total_count;     // Range of block index
};


static void transfer_data(
    const struct reader *reader, read_buffers_t read_buffers,
    int archive, int scon, struct iter_mask *iter, unsigned int data_mask,
    unsigned int block, unsigned int offset, unsigned int count)
{
    /* The write buffer determines how much we write to the socket layer at a
     * time, so a comfortably large buffer is convenient.  Of course, it must be
     * large enough to accomodate a single output line, but that is
     * straightforward. */
    char write_buffer[64 * K];
    unsigned int line_size_out = iter->count * reader->output_size(data_mask);

    bool ok = true;
    while (ok  &&  count > 0)
    {
        /* Read a single timeframe for each id from the archive.  This is
         * normally a single large disk IO block per BPM id. */
        unsigned int samples_read;
        for (unsigned int i = 0; ok  &&  i < iter->count; i ++)
            ok = reader->read_block(
                archive, block, iter->index[i], read_buffers[i], &samples_read);

        /* Transpose the read data into output lines and write out in buffer
         * sized chunks. */
        while (ok  &&  offset < samples_read  &&  count > 0)
        {
            char *p = write_buffer;
            size_t buf_size = 0;
            while (count > 0  &&
                offset < samples_read  &&
                buf_size + line_size_out <= sizeof(write_buffer))
            {
                reader->write_line(
                    iter->count, read_buffers, offset, data_mask, p);
                p += line_size_out;

                count -= 1;
                offset += 1;
                buf_size += line_size_out;
            }
            ok = TEST_write(scon, write_buffer, buf_size);
        }

        block = (block + 1) % reader->block_total_count;
        offset = 0;
    }
}


static void read_data(
    const struct reader *reader, int scon,
    unsigned int data_mask, filter_mask_t read_mask,
    uint64_t start, unsigned int samples)
{
    struct iter_mask iter = { 0 };
    unsigned int block, offset;
    read_buffers_t read_buffers = NULL;
    int archive = -1;
    bool ok =
        reader->timestamp_to_index(start, samples, &block, &offset)  &&
        mask_to_archive(read_mask, &iter)  &&
        lock_buffers(&read_buffers, iter.count)  &&
        TEST_IO(archive = open(archive_filename, O_RDONLY));
    report_socket_error(scon, ok);

    if (ok)
        transfer_data(
            reader, read_buffers, archive, scon,
            &iter, data_mask, block, offset, samples);

    if (read_buffers != NULL)
        unlock_buffers(read_buffers, iter.count);
    if (archive != -1)
        close(archive);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Format specific definitions. */


static bool check_samples(uint64_t available, unsigned int samples)
{
    return
        TEST_OK_(samples <= available,
            "Only %"PRIu64" samples of %u requested available",
            available, samples);
}

static bool timestamp_to_fa_index(
    uint64_t start, unsigned int samples,
    unsigned int *block, unsigned int *offset)
{
    uint64_t available;
    return
        timestamp_to_index(start, &available, block, offset)  &&
        check_samples(available, samples);
}

static bool timestamp_to_d_index(
    uint64_t start, unsigned int samples,
    unsigned int *block, unsigned int *offset)
{
    uint32_t decimation = get_header()->first_decimation;
    uint64_t available;
    return
        timestamp_to_index(start, &available, block, offset)  &&
        DO_(*offset /= decimation; available /= decimation)  &&
        check_samples(available, samples);
}

static bool timestamp_to_dd_index(
    uint64_t start, unsigned int samples,
    unsigned int *block, unsigned int *offset)
{
    return TEST_OK_(false, "not implemented");
}


static bool read_fa_block(
    int archive, unsigned int major_block, unsigned int id,
    void *block, unsigned int *samples)
{
    const struct disk_header *header = get_header();
    *samples = header->major_sample_count;
    int fa_block_size = FA_ENTRY_SIZE * *samples;
    off64_t offset =
        header->major_data_start +
        (uint64_t) header->major_block_size * major_block +
        fa_block_size * id;
    return
        DO_(request_read())  &&
        TEST_IO(lseek(archive, offset, SEEK_SET))  &&
        TEST_read(archive, block, fa_block_size);
}

static bool read_d_block(
    int archive, unsigned int major_block, unsigned int id,
    void *block, unsigned int *samples)
{
    const struct disk_header *header = get_header();
    *samples = header->d_sample_count;
    int fa_block_size = FA_ENTRY_SIZE * header->major_sample_count;
    int d_block_size = sizeof(struct decimated_data) * header->d_sample_count;
    off64_t offset =
        header->major_data_start +
        (uint64_t) header->major_block_size * major_block +
        header->archive_mask_count * fa_block_size +
        d_block_size * id;
    return
        DO_(request_read())  &&
        TEST_IO(lseek(archive, offset, SEEK_SET))  &&
        TEST_read(archive, block, d_block_size);
}

static bool read_dd_block(
    int archive, unsigned int major_block, unsigned int id,
    void *block, unsigned int *samples)
{
    return TEST_OK_(false, "DD not implemented");
}


static void fa_write_line(
    unsigned int count, read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, char *output)
{
    struct fa_entry *p = (struct fa_entry *) output;
    for (unsigned int i = 0; i < count; i ++)
        *p++ = read_buffers[i][offset];
}

static void d_write_line(
    unsigned int count, read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, char *output)
{
    struct fa_entry *p = (struct fa_entry *) output;
    for (unsigned int i = 0; i < count; i ++)
    {
        /* Each input buffer is an array of decimated_data structures which we
         * index by offset, but we then cast this to an array of fa_entry
         * structures to allow the individual fields to be selected by the
         * data_mask. */
        struct fa_entry *input = (struct fa_entry *)
            &((struct decimated_data *) read_buffers[i])[offset];
        if (data_mask & 1)  *p++ = input[0];
        if (data_mask & 2)  *p++ = input[1];
        if (data_mask & 4)  *p++ = input[2];
        if (data_mask & 8)  *p++ = input[3];
    }
}


static unsigned int fa_output_size(unsigned int data_mask)
{
    return FA_ENTRY_SIZE;
}

/* For decimated data the data mask selects individual data fields that are
 * going to be emitted, so we count them here. */
static unsigned int d_output_size(unsigned int data_mask)
{
    unsigned int count =
        ((data_mask >> 0) & 1) + ((data_mask >> 1) & 1) +
        ((data_mask >> 2) & 1) + ((data_mask >> 3) & 1);
    return count * FA_ENTRY_SIZE;
}


static struct reader fa_reader = {
    .timestamp_to_index = timestamp_to_fa_index,
    .read_block = read_fa_block,
    .write_line = fa_write_line,
    .output_size = fa_output_size,
};

static struct reader d_reader = {
    .timestamp_to_index = timestamp_to_d_index,
    .read_block = read_d_block,
    .write_line = d_write_line,
    .output_size = d_output_size,
};

static struct reader dd_reader = {
    .timestamp_to_index = timestamp_to_dd_index,
    .read_block = read_dd_block,
    .write_line = d_write_line,
    .output_size = d_output_size,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read request parsing. */

/* A read request specifies the following:
 *
 *  Data source: normal FA data, decimated or double decimated data.
 *  For decimated data, a field mask is specifed
 *  Mask of BPM ids to be retrieved
 *  Data start point as a timestamp
 *
 * The syntax is very simple (no spaces allowed):
 *
 *  read-request = "R" source "M" filter-mask start "N" samples
 *  start = "T" date-time | "S" seconds [ "." nanoseconds ]
 *  source = "F" | "D" ["D"] ["F" data-mask]
 *  samples = integer
 *  data-mask = integer
 *
 * */

/* Result of parsing a read command. */
struct read_parse {
    filter_mask_t read_mask;        // List of BPMs to be read
    unsigned int samples;           // Requested number of samples
    uint64_t start;                 // Data start (in microseconds into epoch)
    const struct reader *reader;    // Interpretation of data source
    unsigned int data_mask;         // Data mask for D and DD data
};


static bool parse_source(const char **string, struct read_parse *parse)
{
    if (read_char(string, 'F'))
    {
        parse->reader = &fa_reader;
        return true;
    }
    else if (read_char(string, 'D'))
    {
        parse->data_mask = 0xF;     // Default to all fields if no mask
        if (read_char(string, 'D'))
            parse->reader = &dd_reader;
        else
            parse->reader = &d_reader;
        if (read_char(string, 'F'))
            return parse_uint(string, &parse->data_mask);
        else
            return true;
    }
    else
        return TEST_OK_(false, "Invalid source specification");
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
        parse_source(string, parse)  &&
        parse_char(string, 'M')  &&
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
    push_error_handling();      // Popped by report_socket_error()
    if (DO_PARSE("read request", parse_read_request, buf, &parse))
        read_data(
            parse.reader, scon, parse.data_mask,
            parse.read_mask, parse.start, parse.samples);
    else
        report_socket_error(scon, false);
}


bool initialise_reader(const char *archive)
{
    archive_filename = archive;

    /* Initialise dynamic part of reader structures. */
    const struct disk_header *header = get_header();
    fa_reader.block_total_count  = header->major_block_count;
    d_reader.block_total_count   = header->major_block_count;
// these are all bogus!
    dd_reader.block_total_count  = header->major_block_count;

    return initialise_buffer_pool(256);
}
