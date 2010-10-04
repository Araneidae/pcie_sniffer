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

static void initialise_buffer_pool(size_t buffer_size, unsigned int count)
{
    for (unsigned int i = 0; i < count; i ++)
    {
        struct pool_entry *entry =
            malloc(sizeof(struct pool_entry) + buffer_size);
        entry->next = buffer_pool;
        buffer_pool = entry;
    }
    pool_size = count;
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
    /* Reads the requested block from archive into buffer. */
    bool (*read_block)(
        int archive, unsigned int block, unsigned int i,
        void *buffer, unsigned int *samples);
    /* Writes a single line from a list of buffers to an output buffer. */
    void (*write_lines)(
        unsigned int line_count, unsigned int field_count,
        read_buffers_t read_buffers, unsigned int offset,
        unsigned int data_mask, void *output);
    /* The size of a single output value. */
    size_t (*output_size)(unsigned int data_mask);

    unsigned int block_total_count;     // Range of block index
    unsigned int decimation;            // FA samples per read sample
    unsigned int fa_blocks_per_block;   // Index blocks per read block
    unsigned int samples_per_fa_block;  // Samples in a single FA block
};


/* Helper routine to calculate the ceiling of a/b. */
static unsigned int round_up(unsigned int a, unsigned int b)
{
    return (a + b - 1) / b;
}


/* Using the reader parameters converts an index block number and offset into a
 * read block number and offset, and adjusts the available sample count
 * accordingly. */
static void fixup_offset(
    const struct reader *reader, unsigned int ix_block,
    unsigned int *block, unsigned int *offset, uint64_t *available)
{
    *available /= reader->decimation;
    *offset =
        *offset / reader->decimation +
        (ix_block % reader->fa_blocks_per_block) * reader->samples_per_fa_block;
    *block = ix_block / reader->fa_blocks_per_block;
}


/* Converts data block and offset into an index block and data offset.  Note
 * that the computed data offset is still in reader sized samples, to convert to
 * FA samples a further multiplication by reader->decimation is needed. */
static void convert_data_to_index(
    const struct reader *reader,
    unsigned int data_block, unsigned int data_offset,
    unsigned int *ix_block, unsigned int *ix_offset)
{
    *ix_block =
        data_block * reader->fa_blocks_per_block +
        data_offset / reader->samples_per_fa_block;
    *ix_offset = data_offset % reader->samples_per_fa_block;
}


/* Checks that the run of samples from (ix_start,offset) has no gaps.  Here the
 * start is an index block, but the offset is an offset in data points. */
static bool check_run(
    const struct reader *reader,
    unsigned int ix_start, unsigned int offset, unsigned int samples)
{
    /* Convert offset into a data offset into the current index block and
     * compute the total number of index blocks that will need to be read. */
    offset = offset % reader->samples_per_fa_block;
    unsigned int blocks = round_up(
        offset + samples, reader->samples_per_fa_block);
    unsigned int blocks_requested = blocks;
    /* Check whether they represent a contiguous data block. */
    return TEST_OK_(!find_gap(&ix_start, &blocks),
        "Only %u contiguous samples available",
        (blocks_requested - blocks) * reader->samples_per_fa_block - offset);
}


static bool compute_start(
    const struct reader *reader,
    uint64_t start, unsigned int samples, bool only_contiguous,
    unsigned int *block, unsigned int *offset)
{
    uint64_t available;
    unsigned int ix_block;
    return
        /* Convert requested timestamp into a starting index block and FA offset
         * into that block. */
        timestamp_to_index(start, &available, &ix_block, offset)  &&
        /* Convert FA block, offset and available counts into numbers
         * appropriate for our current data type. */
        DO_(fixup_offset(reader, ix_block, block, offset, &available))  &&
        /* Check the requested data set is valid and available. */
        TEST_OK_(samples <= available,
            "Only %"PRIu64" samples of %u requested available",
            available, samples)  &&
        IF_(only_contiguous,
            check_run(reader, ix_block, *offset, samples));
}


static bool send_timestamp(
    const struct reader *reader, int scon,
    unsigned int block, unsigned int offset)
{
    unsigned int ix_block, ix_offset;
    convert_data_to_index(reader, block, offset, &ix_block, &ix_offset);
    const struct data_index *data_index = read_index(ix_block);
    uint64_t timestamp =
        data_index->timestamp +
        (uint64_t) ix_offset * data_index->duration /
            reader->samples_per_fa_block;
    return TEST_write(scon, &timestamp, sizeof(uint64_t));
}



static bool send_gaplist(
    const struct reader *reader, int scon,
    unsigned int block, unsigned int offset, unsigned int samples)
{
    /* First convert block, offset and samples into an index block count. */
    unsigned int samples_per_block = reader->samples_per_fa_block;
    unsigned int ix_start, data_offset;
    convert_data_to_index(reader, block, offset, &ix_start, &data_offset);

    unsigned int ix_count = round_up(samples + data_offset, samples_per_block);
    unsigned int N = get_header()->major_block_count;

    /* Now count the gaps. */
    uint32_t gap_count = 0;
    for (unsigned int ix_block_ = ix_start, ix_count_ = ix_count;
            find_gap(&ix_block_, &ix_count_); )
        gap_count += 1;

    /* Send the gap count and gaps to the client.  We don't bother to write
     * buffer this as this won't happen very often. */
    bool ok = TEST_write(scon, &gap_count, sizeof(uint32_t));
    unsigned int ix_block = ix_start;
    for (unsigned int i = 0;  ok  &&  i <= gap_count;  i ++)
    {
        struct {
            uint32_t data_index;
            uint32_t id_zero;
            uint64_t timestamp;
        } gap_data;
        const struct data_index *data_index = read_index(ix_block);
        gap_data.timestamp = data_index->timestamp;
        gap_data.id_zero = data_index->id_zero;
        if (i == 0)
        {
            /* The first data point needs to be adjusted so that it's the first
             * delivered data point, not the first point in the index block. */
            gap_data.data_index = 0;
            gap_data.id_zero += data_offset * reader->decimation;
            gap_data.timestamp +=
                (uint64_t) data_offset * data_index->duration /
                    reader->samples_per_fa_block;
        }
        else
        {
            /* For subsequent blocks the offset into the data stream will be on
             * an index block boundary, but offset by our start offset. */
            unsigned int blocks = ix_block >= ix_start ?
                ix_block - ix_start : ix_block - ix_start + N;
            gap_data.data_index = blocks * samples_per_block - data_offset;
        }

        ok = TEST_write(scon, &gap_data, sizeof(gap_data));
        find_gap(&ix_block, &ix_count);
    }
    return ok;
}


static bool transfer_data(
    const struct reader *reader, read_buffers_t read_buffers,
    int archive, int scon, struct iter_mask *iter, unsigned int data_mask,
    unsigned int block, unsigned int offset, unsigned int count)
{
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
            /* The write buffer determines how much we write to the socket layer
             * at a time, so a comfortably large buffer is convenient.  Of
             * course, it must be large enough to accomodate a single output
             * line, but that is straightforward. */
            char write_buffer[64 * K];
            /* Enough lines to fill the write buffer, so long as we don't write
             * more than requested and we don't exhaust the read blocks. */
            unsigned int line_count = sizeof(write_buffer) / line_size_out;
            if (count < line_count)
                line_count = count;
            if (offset + line_count > samples_read)
                line_count = samples_read - offset;

            reader->write_lines(
                line_count, iter->count,
                read_buffers, offset, data_mask, write_buffer);
            ok = TEST_write(scon, write_buffer, line_count * line_size_out);

            count -= line_count;
            offset += line_count;
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
    bool only_contiguous, bool want_timestamp, bool gaplist)
{
    unsigned int block, offset;
    struct iter_mask iter = { 0 };
    read_buffers_t read_buffers = NULL;
    int archive = -1;
    bool ok =
        compute_start(
            reader, start, samples, only_contiguous, &block, &offset)  &&
        mask_to_archive(read_mask, &iter)  &&
        lock_buffers(&read_buffers, iter.count)  &&
        TEST_IO(archive = open(archive_filename, O_RDONLY));
    bool write_ok = report_socket_error(scon, ok);

    if (ok  &&  write_ok)
        write_ok =
            IF_(want_timestamp,
                send_timestamp(reader, scon, block, offset))  &&
            IF_(gaplist,
                send_gaplist(reader, scon, block, offset, samples))  &&
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

    unsigned int max_samples_per_block =
        dd_reader.fa_blocks_per_block * dd_reader.samples_per_fa_block;
    size_t offset =
        header->dd_total_count * id +
        max_samples_per_block * major_block;
    if (major_block + 1 == dd_reader.block_total_count)
        /* The last block is quite likely to be short. */
        *samples = header->dd_total_count -
            major_block * max_samples_per_block;
    else
        *samples = max_samples_per_block;
    memcpy(block, dd_area + offset, sizeof(struct decimated_data) * *samples);
    return true;
}


static void fa_write_lines(
    unsigned int line_count, unsigned int field_count,
    read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, void *p)
{
    struct fa_entry *output = (struct fa_entry *) p;
    for (unsigned int l = 0; l < line_count; l ++)
    {
        for (unsigned int i = 0; i < field_count; i ++)
            *output++ = read_buffers[i][offset];
        offset += 1;
    }
}

static void d_write_lines(
    unsigned int line_count, unsigned int field_count,
    read_buffers_t read_buffers, unsigned int offset,
    unsigned int data_mask, void *p)
{
    struct fa_entry *output = (struct fa_entry *) p;
    for (unsigned int l = 0; l < line_count; l ++)
    {
        for (unsigned int i = 0; i < field_count; i ++)
        {
            /* Each input buffer is an array of decimated_data structures which
             * we index by offset, but we then cast this to an array of fa_entry
             * structures to allow the individual fields to be selected by the
             * data_mask. */
            struct fa_entry *input = (struct fa_entry *)
                &((struct decimated_data *) read_buffers[i])[offset];
            if (data_mask & 1)  *output++ = input[0];
            if (data_mask & 2)  *output++ = input[1];
            if (data_mask & 4)  *output++ = input[2];
        }
        offset += 1;
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
        ((data_mask >> 2) & 1);
    return count * FA_ENTRY_SIZE;
}


static struct reader fa_reader = {
    .read_block = read_fa_block,
    .write_lines = fa_write_lines,
    .output_size = fa_output_size,
    .decimation = 1,
    .fa_blocks_per_block = 1,
};

static struct reader d_reader = {
    .read_block = read_d_block,
    .write_lines = d_write_lines,
    .output_size = d_output_size,
    .fa_blocks_per_block = 1,
};

static struct reader dd_reader = {
    .read_block = read_dd_block,
    .write_lines = d_write_lines,
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
 *  options = [ "T" ] [ "G" ] [ "C" ]
 *
 * The options can only appear in the order given and have the following
 * meanings:
 *
 *  T   Send timestamp at head of dataset
 *  G   Send gap list at end of data capture
 *  C   Ensure no gaps in selected dataset, fail if any
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
    bool gaplist;                   // Send gaplist after data
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
        parse->data_mask = 7;       // Default to all fields if no mask
        if (read_char(string, 'D'))
            parse->reader = &dd_reader;
        else
            parse->reader = &d_reader;
        if (read_char(string, 'F'))
            return
                parse_uint(string, &parse->data_mask)  &&
                TEST_OK_(0 < parse->data_mask  &&  parse->data_mask <= 7,
                    "Invalid decimated data fields: %x", parse->data_mask);
        else
            return true;
    }
    else
        return FAIL_("Invalid source specification");
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
        ok = FAIL_("Expected T or S for timestamp");
    ok = ok  &&  TEST_OK_(ts.tv_sec > 0, "Timestamp ridiculously early");
    *start = 1000000 * (uint64_t) ts.tv_sec + ts.tv_nsec / 1000;
    return ok;
}


/* options = [ "T" ] [ "G" ] [ "C" ] . */
static bool parse_options(const char **string, struct read_parse *parse)
{
    parse->timestamp = read_char(string, 'T');
    parse->gaplist = read_char(string, 'G');
    parse->only_contiguous = read_char(string, 'C');
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
            parse.only_contiguous, parse.timestamp, parse.gaplist);
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
    fa_reader.samples_per_fa_block  = header->major_sample_count;

    d_reader.decimation             = header->first_decimation;
    d_reader.block_total_count      = header->major_block_count;
    d_reader.samples_per_fa_block   = header->d_sample_count;

    dd_reader.decimation =
        header->first_decimation * header->second_decimation;
    dd_reader.fa_blocks_per_block =
        buffer_size / sizeof(struct decimated_data) / header->dd_sample_count;
    dd_reader.block_total_count =
        round_up(header->major_block_count, dd_reader.fa_blocks_per_block);
    dd_reader.samples_per_fa_block = header->dd_sample_count;

    initialise_buffer_pool(buffer_size, 256);
    return true;
}
