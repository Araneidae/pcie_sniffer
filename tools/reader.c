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

static unsigned int pool_size;
static struct pool_entry *buffer_pool = NULL;

typedef struct fa_entry **read_buffers_t;

static bool lock_buffers(read_buffers_t *buffers, unsigned int count)
{
    bool ok;
    LOCK(buffer_lock);
    ok = TEST_OK_(count <= pool_size, "Read too busy");
    if (ok)
    {
        pool_size -= count;
        *buffers = malloc(count * sizeof(struct fa_entry *));
        for (unsigned int i = 0; i < count; i ++)
        {
            struct pool_entry *entry = buffer_pool;
            buffer_pool = entry->next;
            (*buffers)[i] = entry->buffer;
        }
    }
    UNLOCK(buffer_lock);
    return ok;
}

static void unlock_buffers(read_buffers_t buffers, unsigned int count)
{
    LOCK(buffer_lock);
    for (unsigned int i = 0; i < count; i ++)
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

static bool initialise_buffer_pool(size_t buffer_size, unsigned int count)
{
    for (unsigned int i = 0; i < count; i ++)
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
    /* Adjusts computed initial offset according to decimation. */
    bool (*fixup_offset)(
        unsigned int *block, unsigned int *offset, uint64_t *available);
    /* Reads the requested block from archive into buffer. */
    bool (*read_block)(
        int archive, unsigned int block, unsigned int i,
        void *buffer, unsigned int *samples);
    /* Writes a single line from a list of buffers to an output buffer. */
    void (*write_line)(
        unsigned int count, read_buffers_t read_buffers, unsigned int offset,
        unsigned int data_mask, struct fa_entry *output);
    /* The size of a single output value. */
    size_t (*output_size)(unsigned int data_mask);

    unsigned int block_total_count;     // Range of block index
    unsigned int decimation;            // Data decimation
    unsigned int fa_blocks_per_block;   // Number of index blocks per block
    unsigned int max_samples_per_block; // Maximum number of samples per block
};


/* Helper routine to calculate the ceiling of a/b. */
static unsigned int round_up(unsigned int a, unsigned int b)
{
    return (a + b - 1) / b;
}


static bool check_run(
    const struct reader *reader,
    unsigned int start, unsigned int samples, unsigned int offset)
{
    /* Compute the total number of index blocks that will need to be read. */
    unsigned int block_size = get_header()->major_sample_count;
    unsigned int blocks =
        round_up(offset + samples * reader->decimation, block_size);
    unsigned int d_id0;
    int64_t d_t;
    /* Check whether they represent a contiguous data block. */
    unsigned int available = check_contiguous(start, blocks, &d_id0, &d_t);
    return TEST_OK_(available == blocks,
        "Only %u contiguous samples available. Gap: id0 = %d, dt = %"PRId64"us",
        (available * block_size - offset) / reader->decimation, d_id0, d_t);
}


static bool compute_start(
    const struct reader *reader,
    uint64_t start, unsigned int samples, bool only_contiguous,
    unsigned int *block, unsigned int *offset, uint64_t *timestamp)
{
    uint64_t available;
    unsigned int fa_block;
    return
        /* Convert requested timestamp into a starting index block and FA offset
         * into that block. */
        timestamp_to_index(
            start, &available, &fa_block, offset, timestamp)  &&
        DO_(*block = fa_block)  &&
        /* Convert FA block, offset and available counts into numbers
         * appropriate for our current data type. */
        reader->fixup_offset(block, offset, &available)  &&
        /* Check the requested data block is valid and available. */
        TEST_OK_(samples <= available,
            "Only %"PRIu64" samples of %u requested available",
            available, samples)  &&
        IF_(only_contiguous,
            check_run(reader, fa_block, samples, *offset));
}


static bool transfer_data(
    const struct reader *reader, read_buffers_t read_buffers,
    int archive, int scon, struct iter_mask *iter, unsigned int data_mask,
    unsigned int block, unsigned int offset, unsigned int count)
{
    /* The write buffer determines how much we write to the socket layer at a
     * time, so a comfortably large buffer is convenient.  Of course, it must be
     * large enough to accomodate a single output line, but that is
     * straightforward. */
    char write_buffer[64 * K];
    size_t line_size_out = iter->count * reader->output_size(data_mask);

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
                    iter->count, read_buffers, offset, data_mask,
                    (struct fa_entry *) p);
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
    return ok;
}


static bool read_data(
    const struct reader *reader, int scon,
    unsigned int data_mask, filter_mask_t read_mask,
    uint64_t start, unsigned int samples,
    bool only_contiguous, bool want_timestamp)
{
    unsigned int block, offset;
    uint64_t timestamp;
    struct iter_mask iter = { 0 };
    read_buffers_t read_buffers = NULL;
    int archive = -1;
    bool ok =
        compute_start(
            reader, start, samples, only_contiguous,
            &block, &offset, &timestamp)  &&
        mask_to_archive(read_mask, &iter)  &&
        lock_buffers(&read_buffers, iter.count)  &&
        TEST_IO(archive = open(archive_filename, O_RDONLY));
    bool write_ok = report_socket_error(scon, ok);

    if (ok  &&  write_ok)
        write_ok =
            IF_(want_timestamp,
                TEST_write(scon, &timestamp, sizeof(uint64_t)))  &&
            transfer_data(
                reader, read_buffers, archive, scon,
                &iter, data_mask, block, offset, samples);

    if (read_buffers != NULL)
        unlock_buffers(read_buffers, iter.count);
    if (archive != -1)
        close(archive);

    return write_ok;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Format specific definitions. */

/* Forward declaration of the three reader definitions. */
static struct reader fa_reader;
static struct reader d_reader;
static struct reader dd_reader;


static bool fixup_fa_offset(
    unsigned int *block, unsigned int *offset, uint64_t *available)
{
    return true;
}

static bool fixup_d_offset(
    unsigned int *block, unsigned int *offset, uint64_t *available)
{
    *offset    /= d_reader.decimation;
    *available /= d_reader.decimation;
    return true;
}

static bool fixup_dd_offset(
    unsigned int *block, unsigned int *offset, uint64_t *available)
{
    *offset    /= dd_reader.decimation;
    *available /= dd_reader.decimation;

    /* DD blocks are read as enough FA blocks to fill a read buffer -- this is
     * block_divider, and each block thus contains (up to)
     * max_samples_per_block, which is what we read. */
    unsigned int block_divider = dd_reader.fa_blocks_per_block;
    unsigned int fa_block_size = get_header()->dd_sample_count;
    *offset += fa_block_size * (*block % block_divider);
    *block  /= block_divider;
    return true;
}


static bool read_fa_block(
    int archive, unsigned int major_block, unsigned int id,
    void *block, unsigned int *samples)
{
    const struct disk_header *header = get_header();
    *samples = header->major_sample_count;
    size_t fa_block_size = FA_ENTRY_SIZE * *samples;
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
    size_t fa_block_size = FA_ENTRY_SIZE * header->major_sample_count;
    size_t d_block_size =
        sizeof(struct decimated_data) * header->d_sample_count;
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
    const struct disk_header *header = get_header();
    const struct decimated_data *dd_area = get_dd_area();

    size_t offset =
        header->dd_total_count * id +
        dd_reader.max_samples_per_block * major_block;
    if (major_block + 1 == dd_reader.block_total_count)
        /* The last block is quite likely to be short. */
        *samples = header->dd_total_count -
            major_block * dd_reader.max_samples_per_block;
    else
        *samples = dd_reader.max_samples_per_block;
    memcpy(block, dd_area + offset, sizeof(struct decimated_data) * *samples);
    return true;
}


static void fa_write_line(
    unsigned int count, read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, struct fa_entry *output)
{
    for (unsigned int i = 0; i < count; i ++)
        *output++ = read_buffers[i][offset];
}

static void d_write_line(
    unsigned int count, read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, struct fa_entry *output)
{
    for (unsigned int i = 0; i < count; i ++)
    {
        /* Each input buffer is an array of decimated_data structures which we
         * index by offset, but we then cast this to an array of fa_entry
         * structures to allow the individual fields to be selected by the
         * data_mask. */
        struct fa_entry *input = (struct fa_entry *)
            &((struct decimated_data *) read_buffers[i])[offset];
        if (data_mask & 1)  *output++ = input[0];
        if (data_mask & 2)  *output++ = input[1];
        if (data_mask & 4)  *output++ = input[2];
        if (data_mask & 8)  *output++ = input[3];
    }
}


static size_t fa_output_size(unsigned int data_mask)
{
    return FA_ENTRY_SIZE;
}

/* For decimated data the data mask selects individual data fields that are
 * going to be emitted, so we count them here. */
static size_t d_output_size(unsigned int data_mask)
{
    unsigned int count =
        ((data_mask >> 0) & 1) + ((data_mask >> 1) & 1) +
        ((data_mask >> 2) & 1) + ((data_mask >> 3) & 1);
    return count * FA_ENTRY_SIZE;
}


static struct reader fa_reader = {
    .fixup_offset = fixup_fa_offset,
    .read_block = read_fa_block,
    .write_line = fa_write_line,
    .output_size = fa_output_size,
    .decimation = 1,
    .fa_blocks_per_block = 1,
};

static struct reader d_reader = {
    .fixup_offset = fixup_d_offset,
    .read_block = read_d_block,
    .write_line = d_write_line,
    .output_size = d_output_size,
    .fa_blocks_per_block = 1,
};

static struct reader dd_reader = {
    .fixup_offset = fixup_dd_offset,
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
 *  read-request = "R" source "M" filter-mask start "N" samples options
 *  start = "T" date-time | "S" seconds [ "." nanoseconds ]
 *  source = "F" | "D" ["D"] ["F" data-mask]
 *  samples = integer
 *  data-mask = integer
 *  options = [ "C" ] [ "T" ]
 *
 * The options can only appear in the order given and have the following
 * meanings:
 *
 *  C   Ensure no gaps in selected dataset, fail if any
 *  T   Send timestamp at head of dataset
 */

/* Result of parsing a read command. */
struct read_parse {
    filter_mask_t read_mask;        // List of BPMs to be read
    unsigned int samples;           // Requested number of samples
    uint64_t start;                 // Data start (in microseconds into epoch)
    const struct reader *reader;    // Interpretation of data source
    unsigned int data_mask;         // Data mask for D and DD data
    bool only_contiguous;           // Only contiguous data acceptable
    bool timestamp;                 // Send timestamp at start of data
};


/* source = "F" | "D" [ "D" ] [ "F" data-mask ] . */
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
            return
                parse_uint(string, &parse->data_mask)  &&
                TEST_OK_(0 < parse->data_mask  &&  parse->data_mask <= 15,
                    "Invalid decimated data fields: %x", parse->data_mask);
        else
            return true;
    }
    else
        return TEST_OK_(false, "Invalid source specification");
}


/* start = "T" datetime | "S" seconds . */
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


/* options = [ "C" ] [ "T" ] . */
static bool parse_options(const char **string, struct read_parse *parse)
{
    parse->only_contiguous = read_char(string, 'C');
    parse->timestamp = read_char(string, 'T');
    return true;
}


/* read-request = "R" source "M" mask start "N" samples options . */
static bool parse_read_request(const char **string, struct read_parse *parse)
{
    return
        parse_char(string, 'R')  &&
        parse_source(string, parse)  &&
        parse_char(string, 'M')  &&
        parse_mask(string, parse->read_mask)  &&
        parse_start(string, &parse->start)  &&
        parse_char(string, 'N')  &&
        parse_uint(string, &parse->samples)  &&
        parse_options(string, parse);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read processing. */


/* Convert timestamp into block index. */
bool process_read(int scon, const char *buf)
{
    struct read_parse parse;
    push_error_handling();      // Popped by report_socket_error()
    if (DO_PARSE("read request", parse_read_request, buf, &parse))
        return read_data(
            parse.reader, scon, parse.data_mask,
            parse.read_mask, parse.start, parse.samples,
            parse.only_contiguous, parse.timestamp);
    else
        return report_socket_error(scon, false);
}


bool initialise_reader(const char *archive)
{
    archive_filename = archive;

    const struct disk_header *header = get_header();
    /* Make the buffer size large enough for a complete FA major block for one
     * BPM id. */
    size_t buffer_size = FA_ENTRY_SIZE * header->major_sample_count;

    /* Initialise dynamic part of reader structures. */
    fa_reader.block_total_count     = header->major_block_count;
    fa_reader.max_samples_per_block = header->major_sample_count;

    d_reader.decimation             = header->first_decimation;
    d_reader.block_total_count      = header->major_block_count;
    d_reader.max_samples_per_block  = header->d_sample_count;

    dd_reader.decimation =
        header->first_decimation * header->second_decimation;
    dd_reader.fa_blocks_per_block =
        buffer_size / sizeof(struct decimated_data) / header->dd_sample_count;
    dd_reader.block_total_count =
        round_up(header->major_block_count, dd_reader.fa_blocks_per_block);
    dd_reader.max_samples_per_block =
        buffer_size / sizeof(struct decimated_data);

    return initialise_buffer_pool(buffer_size, 256);
}
