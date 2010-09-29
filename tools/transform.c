/* Data transposition. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "transform.h"
#include "disk_writer.h"
#include "locking.h"

#include "disk.h"


// !!! should be disk header parameter
#define TIMESTAMP_IIR   0.1

/* Allow up to 1ms delta before reporting a data capture gap. */
#define MAX_DELTA_T     1000

/* We skip this many old index blocks that are still within range.  This is a
 * simple heuristic to avoid early blocks being overwritten as we're reading
 * them. */
#define INDEX_SKIP      2


/* Archiver header with core parameter. */
static struct disk_header *header;
/* Archiver index. */
static struct data_index *data_index;
/* Area to write DD data. */
static struct decimated_data *dd_area;

/* Numbers of normal and decimated samples in a single input block. */
static unsigned int input_frame_count;
static unsigned int input_decimation_count;

/* This lock guards access to header->current_major_block, or to be precise,
 * enforces the invariant described here.  The transform thread has full
 * unconstrained access to this variable, but only updates it under this lock.
 * All major blocks other than current_major_block are valid for reading from
 * disk, the current block is either being worked on or being written to disk.
 * The request_read() function ensures that the previously current block is
 * written and therefore is available. */
DECLARE_LOCKING(transform_lock);



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffered IO support. */

/* Double-buffered block IO. */

static void *buffers[2];           // Two major buffers to receive data
static unsigned int current_buffer; // Index of buffer currently receiving data
static unsigned int fa_offset;     // Current sample count into current block
static unsigned int d_offset;      // Current decimated sample count


static inline struct fa_entry * fa_block(unsigned int id)
{
    return buffers[current_buffer] + fa_data_offset(header, fa_offset, id);
}


static inline struct decimated_data * d_block(unsigned int id)
{
    return buffers[current_buffer] + d_data_offset(header, d_offset, id);
}


/* Advances the offset pointer within an minor block by the number of bytes
 * written, returns true iff the block is now full. */
static bool advance_block(void)
{
    fa_offset += input_frame_count;
    d_offset += input_frame_count / header->first_decimation;
    return fa_offset >= header->major_sample_count;
}


/* Called if the block is to be discarded. */
static void reset_block(void)
{
    fa_offset = 0;
    d_offset = 0;
}


/* Writes the currently written major block to disk at the current offset. */
static void write_major_block(void)
{
    off64_t offset = header->major_data_start +
        (off64_t) header->current_major_block * header->major_block_size;
    schedule_write(offset, buffers[current_buffer], header->major_block_size);

    current_buffer = 1 - current_buffer;
    reset_block();
}


/* Initialises IO buffers for the given minor block size. */
static void initialise_io_buffer(void)
{
    for (unsigned int i = 0; i < 2; i ++)
        buffers[i] = valloc(header->major_block_size);

    current_buffer = 0;
    fa_offset = 0;
    d_offset = 0;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block transpose. */

/* To make reading of individual BPMs more efficient (the usual usage) we
 * transpose frames into individual BPMs until we've assembled a complete
 * collection of disk blocks (determined by output_block_size). */


static void transpose_column(
    const struct fa_entry *input, struct fa_entry *output)
{
    for (unsigned int i = input_frame_count; i > 0; i--)
    {
        *output ++ = *input;
        input += FA_ENTRY_COUNT;
    }
}


/* Processes a single input block of FA sniffer frames.  Each BPM is written to
 * its own output block.  True is returned iff the transposed buffer set is full
 * and is ready to be written out. */
static void transpose_block(const void *read_block)
{
    /* For the moment forget about being too clever about the impact of
     * transposing data on the cache.  We copy one column at a time. */
    unsigned int written = 0;
    for (unsigned int id = 0; id < FA_ENTRY_COUNT; id ++)
    {
        if (test_mask_bit(header->archive_mask, id))
        {
            transpose_column(
                read_block + FA_ENTRY_SIZE * id, fa_block(written));
            written += 1;
        }
    }
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Single data decimation. */



/* Converts a column of N FA entries into a single entry by computing the mean,
 * min, max and standard deviation of the column. */
static void decimate_column_one(
    const struct fa_entry *input, struct decimated_data *output, unsigned int N)
{
    int64_t sumx = 0, sumy = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN;
    int32_t miny = INT32_MAX, maxy = INT32_MIN;
    const struct fa_entry *in = input;
    for (unsigned int i = 0; i < N; i ++)
    {
        int32_t x = in->x;
        int32_t y = in->y;
        sumx += x;
        sumy += y;
        if (x < minx)   minx = x;
        if (maxx < x)   maxx = x;
        if (y < miny)   miny = y;
        if (maxy < y)   maxy = y;
        in += FA_ENTRY_COUNT;
    }
    output->min.x = minx;    output->max.x = maxx;
    output->min.y = miny;    output->max.y = maxy;
    output->mean.x = (int32_t) (sumx / N);
    output->mean.y = (int32_t) (sumy / N);
}

static void decimate_column(
    const struct fa_entry *input, struct decimated_data *output)
{
    for (unsigned int i = 0; i < input_decimation_count; i ++)
    {
        decimate_column_one(input, output, header->first_decimation);
        input += header->first_decimation * FA_ENTRY_COUNT;
        output += 1;
    }
}


static void decimate_block(const void *read_block)
{
    unsigned int written = 0;
    for (unsigned int id = 0; id < FA_ENTRY_COUNT; id ++)
    {
        if (test_mask_bit(header->archive_mask, id))
        {
            decimate_column(read_block + FA_ENTRY_SIZE * id, d_block(written));
            written += 1;
        }
    }
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Double data decimation. */

/* Current offset into DD data area. */
unsigned int dd_offset;


/* Similar to decimate_column above, but condenses already decimated data by
 * further decimation.  In this case the algorithms are somewhat different. */
static void decimate_decimation(
    const struct decimated_data *input, struct decimated_data *output,
    unsigned int N)
{
    int64_t sumx = 0, sumy = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN;
    int32_t miny = INT32_MAX, maxy = INT32_MIN;
    for (unsigned int i = 0; i < N; i ++, input ++)
    {
        sumx += input->mean.x;
        sumy += input->mean.y;
        if (input->min.x < minx)     minx = input->min.x;
        if (maxx < input->max.x)     maxx = input->max.x;
        if (input->min.y < miny)     miny = input->min.y;
        if (maxy < input->max.y)     maxy = input->max.y;
    }
    output->mean.x = (int32_t) (sumx / N);
    output->mean.y = (int32_t) (sumy / N);
    output->min.x = minx;
    output->max.x = maxx;
    output->min.y = miny;
    output->max.y = maxy;
}



/* In this case we work on decimated data sorted in the d_block and we write to
 * the in memory DD block. */
static void double_decimate_block(void)
{
    /* Note that we look backwards in time one second_decimation block to pick
     * up the data to be decimated here. */
    const struct decimated_data *input = d_block(0) - header->second_decimation;
    struct decimated_data *output = dd_area + dd_offset;

    unsigned int written = 0;
    for (unsigned int id = 0; id < FA_ENTRY_COUNT; id ++)
    {
        if (test_mask_bit(header->archive_mask, id))
        {
            decimate_decimation(input, output, header->second_decimation);
            input += header->d_sample_count;
            output += header->dd_total_count;
            written += 1;
        }
    }

    dd_offset = (dd_offset + 1) % header->dd_total_count;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Index maintenance. */

/* Number of timestamps we expect to see in a single major block. */
static unsigned int timestamp_count;
/* Array of timestamps for timestamp estimation, stored relative to the first
 * timestamp. */
static uint64_t first_timestamp;
static int *timestamp_array;
/* Current index into the timestamp array as we fill it. */
static unsigned int timestamp_index = 0;


/* Adds a minor block to the timestamp array. */
static void index_minor_block(const void *block, struct timespec *ts)
{
    /* Convert timestamp to our working representation in microseconds in the
     * current epoch. */
    uint64_t timestamp = 1000000 * (uint64_t) ts->tv_sec + ts->tv_nsec / 1000;

    if (timestamp_index == 0)
    {
        first_timestamp = timestamp;
        /* For the very first index record the first id 0 field. */
        data_index[header->current_major_block].id_zero =
            ((struct fa_entry *) block)[0].x;
    }

    timestamp_array[timestamp_index] = (int) (timestamp - first_timestamp);
    timestamp_index += 1;
}


/* Called when a major block is complete, complete the index entry. */
static void advance_index(void)
{
    /* Fit a straight line through the timestamps and compute the timestamp at
     * the beginning of the segment. */
    int64_t sum_x = 0;
    int64_t sum_xt = 0;
    for (unsigned int i = 0; i < timestamp_count; i ++)
    {
        int t = 2*i - timestamp_count + 1;
        int64_t x = timestamp_array[i];
        sum_xt += x * t;
        sum_x  += x;
    }
    /* Compute sum_t2 = SUM t**2 through the iteration above, but unwinding the
     * algebra and using summation formulae
     *      SUM_{i=1..N} i   = N(N+1)/2
     *      SUM_{i=1..N} i*i = N(N+1)(2N+1)/6
     * we get sum_t2 = N(N*N-1)/3 . */
    uint64_t sum_t2 =
        ((uint64_t) timestamp_count * timestamp_count - 1) *
        timestamp_count / 3;

    struct data_index *ix = &data_index[header->current_major_block];
    /* Duration is "slope" calculated from fit above over an interval of
     * 2*timestamp_count. */
    ix->duration = (uint32_t) (2 * timestamp_count * sum_xt / sum_t2);
    /* Starting timestamp is computed at t=-timestamp_count-1 from centre. */
    ix->timestamp = first_timestamp +
        sum_x / timestamp_count - (timestamp_count + 1) * sum_xt / sum_t2;

    /* For the last duration we run an IIR to smooth out the bumps in our
     * timestamp calculations.  This gives us another digit or so. */
    header->last_duration = (uint32_t) round(
        ix->duration * TIMESTAMP_IIR +
        header->last_duration * (1 - TIMESTAMP_IIR));

    /* All done, advance the block index and reset our index. */
    header->current_major_block =
        (header->current_major_block + 1) % header->major_block_count;
    timestamp_index = 0;
}


/* Discard work so far, called when we see a gap. */
static void reset_index(void)
{
    timestamp_index = 0;
}


static void initialise_index(void)
{
    timestamp_count = header->major_sample_count / input_frame_count;
    timestamp_array = malloc(sizeof(int) * timestamp_count);
    timestamp_index = 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interlocked access. */


bool timestamp_to_index(
    uint64_t timestamp, uint64_t *samples_available,
    unsigned int *major_block, unsigned int *offset)
{
    bool ok;
    LOCK(transform_lock);

    unsigned int N = header->major_block_count;
    unsigned int current = header->current_major_block;

    /* Binary search to find major block corresponding to timestamp.  Note that
     * the high block is never inspected, which is just as well, as the current
     * block is invariably invalid. */
    unsigned int low  = (current + 1 + INDEX_SKIP) % N;
    unsigned int high = current;
    while ((low + 1) % N != high)
    {
        unsigned int mid;
        if (low < high)
            mid = (low + high) / 2;
        else
            mid = ((low + high + N) / 2) % N;
        if (timestamp < data_index[mid].timestamp)
            high = mid;
        else
            low = mid;
    }

    uint64_t block_start = data_index[low].timestamp;
    unsigned int duration = data_index[low].duration;
    ok = TEST_OK_(duration > 0, "Timestamp not in index");
    if (ok)
    {
        /* Compute the raw offset.  If we fall off the end of the selected block
         * (perhaps there's a capture gap) simply skip to the following block.
         * Note that this can push us to an invalid timestamp. */
        uint64_t raw_offset =
            (timestamp - block_start) * header->major_sample_count /
            data_index[low].duration;
        if (raw_offset >= header->major_sample_count)
        {
            low = (low + 1) % N;
            raw_offset = 0;
        }

        /* Store the results and validate the timestamp and sample count. */
        *major_block = low;
        *offset = (unsigned int) raw_offset;
        unsigned int block_count =
            current > low ? current - low : N - low + current;
        *samples_available =
            (uint64_t) block_count * header->major_sample_count - raw_offset;
        ok =
            TEST_OK_(low != current, "Timestamp too late")  &&
            TEST_OK_(timestamp >= block_start, "Timestamp too early");
    }

    UNLOCK(transform_lock);
    return ok;
}


bool find_gap(unsigned int *start, unsigned int *blocks)
{
    struct data_index *ix = &data_index[*start];
    uint64_t timestamp = ix->timestamp + ix->duration;
    uint32_t id_zero   = ix->id_zero + header->major_sample_count;
    while (*blocks > 1)
    {
        *blocks -= 1;
        *start += 1;
        if (*start == header->major_block_count)
            *start = 0;

        ix = &data_index[*start];
        int64_t delta_t = ix->timestamp - timestamp;
        if (ix->id_zero != id_zero  ||
            delta_t < -MAX_DELTA_T  ||  MAX_DELTA_T < delta_t)
            return true;

        timestamp = ix->timestamp + ix->duration;
        id_zero = ix->id_zero + header->major_sample_count;
    }
    return false;
}


const struct data_index * read_index(unsigned int ix)
{
    return &data_index[ix];
}

const struct disk_header *get_header(void)
{
    return header;
}

const struct decimated_data * get_dd_area(void)
{
    return dd_area;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level control. */


/* Processes a single block of raw frames read from the internal circular
 * buffer, transposing for efficient read and generating decimations as
 * appropriate.  Schedules write to disk as appropriate when buffer is full
 * enough. */
void process_block(const void *block, struct timespec *ts)
{
    if (block)
    {
        index_minor_block(block, ts);
        transpose_block(block);
        decimate_block(block);
        bool must_write = advance_block();
        if (fa_offset % (
                header->first_decimation * header->second_decimation) == 0)
            double_decimate_block();
        if (must_write)
        {
            LOCK(transform_lock);
            write_major_block();
            advance_index();
            UNLOCK(transform_lock);
        }
    }
    else
    {
        /* If we see a gap in the block then discard all the work we've done so
         * far. */
        reset_block();
        reset_index();
        dd_offset = header->current_major_block * header->dd_sample_count;
    }
}


bool initialise_transform(
    struct disk_header *header_, struct data_index *data_index_,
    struct decimated_data *dd_area_)
{
    header = header_;
    data_index = data_index_;
    dd_area = dd_area_;
    input_frame_count = header->input_block_size / FA_FRAME_SIZE;
    input_decimation_count = input_frame_count / header->first_decimation;
    dd_offset = header->current_major_block * header->dd_sample_count;

    initialise_io_buffer();
    initialise_index();
    return true;
}
