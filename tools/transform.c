/* Data transposition. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <aio.h>
#include <math.h>
#include <xmmintrin.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "transform.h"
#include "disk.h"



/* Archiver header with core parameter. */
static struct disk_header *header;

static int input_frame_count;
static int input_decimation_count;




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffered IO support. */

/* Double-buffered block IO. */

struct io_buffer {
    void *buffers[2];           // Two major buffers to receive data
    int current_buffer;         // Index of buffer currently receiving data
    unsigned int fa_offset;     // Current sample count into current block
    unsigned int d_offset;      // Current decimated sample count
    struct aiocb aiocb;         // IO control block used for writing data
    bool io_waiting;            // Set while AIO might be in progress
};

struct io_buffer io_buffer;


static inline struct fa_entry * fa_block(int id)
{
    return
        io_buffer.buffers[io_buffer.current_buffer] +
        fa_data_offset(header, io_buffer.fa_offset, id);
}


static inline struct decimated_data * d_block(int id)
{
    return
        io_buffer.buffers[io_buffer.current_buffer] +
        d_data_offset(header, io_buffer.d_offset, id);
}


/* Advances the offset pointer within an minor block by the number of bytes
 * written, returns true iff the block is now full. */
static bool advance_block(void)
{
    io_buffer.fa_offset += input_frame_count;
    io_buffer.d_offset += input_frame_count / header->first_decimation;
    return io_buffer.fa_offset >= header->major_sample_count;
}


/* Called if the block is to be discarded. */
static void reset_block(void)
{
    io_buffer.fa_offset = 0;
    io_buffer.d_offset = 0;
}


/* Writes the currently written major block to disk at the current offset. */
static void write_major_block(void)
{
    off64_t offset = header->major_data_start +
        (off64_t) header->current_major_block * header->major_block_size;
    header->current_major_block =
        (header->current_major_block + 1) % header->major_block_count;

    if (io_buffer.io_waiting)
    {
        const struct aiocb *aiocb_list[] = { &io_buffer.aiocb };
        ASSERT_IO(aio_suspend(aiocb_list, 1, NULL));
    }

    io_buffer.aiocb.aio_buf = io_buffer.buffers[io_buffer.current_buffer];
    io_buffer.aiocb.aio_offset = offset;
    ASSERT_IO(aio_write(&io_buffer.aiocb));
    io_buffer.io_waiting = true;

    io_buffer.fa_offset = 0;
    io_buffer.d_offset = 0;
    io_buffer.current_buffer = 1 - io_buffer.current_buffer;
}


/* Initialises IO buffers for the given minor block size. */
static void initialise_io_buffer(int output_file)
{
    for (int i = 0; i < 2; i ++)
        io_buffer.buffers[i] = valloc(header->major_block_size);

    io_buffer.current_buffer = 0;
    io_buffer.fa_offset = 0;
    io_buffer.d_offset = 0;
    io_buffer.io_waiting = false;

    io_buffer.aiocb.aio_fildes = output_file;
    io_buffer.aiocb.aio_nbytes = header->major_block_size;
    io_buffer.aiocb.aio_sigevent.sigev_notify = SIGEV_NONE;
    io_buffer.aiocb.aio_reqprio = 0;
    io_buffer.aiocb.aio_lio_opcode = 0;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block transpose. */

/* To make reading of individual BPMs more efficient (the usual usage) we
 * transpose frames into individual BPMs until we've assembled a complete
 * collection of disk blocks (determined by output_block_size). */


static void transpose_column(
    const struct fa_entry *input, struct fa_entry *output)
{
    COMPILE_ASSERT(sizeof(__m64) == FA_ENTRY_SIZE);
    for (int i = 0; i < input_frame_count; i++)
    {
        /* This call is an SSE intrinsic, defined in xmmintrin.h, only enabled
         * if -msse specified on the command line.  The effect of this is to use
         * the MMX register for transferring a single FA entry, but more to the
         * point, write combining is used on the output.  This means that the
         * output block is never fetched into cache, which should significantly
         * speed up processing.
         *    Alas, the documentation for this is pretty poor.
         *
         * References include:
         *  http://lwn.net/Articles/255364/
         *      Ulrich Drepper on memory optimisation
         *  http://www.redjam.com/codeplay/documentation/intrinsics.html
         *      Lists intrinsics for another compiler
         *  http://math.nju.edu.cn/help/mathhpc/doc/intel/cc/
         *  mergedProjects/intref_cls/
         *      Documents Intel intrinsics. */
        _mm_stream_pi((__m64 *) output, *(__m64 *) input);

        output += 1;
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
    int written = 0;
    for (int id = 0; id < FA_ENTRY_COUNT; id ++)
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
    const struct fa_entry *input, struct decimated_data *output, int N)
{
    int64_t sumx = 0, sumy = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN;
    int32_t miny = INT32_MAX, maxy = INT32_MIN;
    for (int i = 0; i < N; i ++)
    {
        int32_t x = input[i * FA_ENTRY_COUNT].x;
        int32_t y = input[i * FA_ENTRY_COUNT].y;
        sumx += x;
        sumy += y;
        if (x < minx)   minx = x;
        if (maxx < x)   maxx = x;
        if (y < miny)   miny = y;
        if (maxy < y)   maxy = y;
    }
    output->minx = minx;    output->maxx = maxx;
    output->miny = miny;    output->maxy = maxy;
    double meanx = (double) sumx / N;
    double meany = (double) sumy / N;
    output->meanx = (int32_t) round(meanx);
    output->meany = (int32_t) round(meany);

    /* For numerically stable computation of variance we take a second pass over
     * the data. */
    double sumvarx = 0, sumvary = 0;
    for (int i = 0; i < N; i ++)
    {
        int32_t x = input[i * FA_ENTRY_COUNT].x;
        int32_t y = input[i * FA_ENTRY_COUNT].y;
        sumvarx += (x - meanx) * (x - meanx);
        sumvary += (y - meany) * (y - meany);
    }
    output->stdx = (int32_t) round(sqrt(sumvarx / N));
    output->stdy = (int32_t) round(sqrt(sumvary / N));
}

static void decimate_column(
    const struct fa_entry *input, struct decimated_data *output)
{
    for (int i = 0; i < input_decimation_count; i ++)
    {
        decimate_column_one(input, output, header->first_decimation);
        input += header->first_decimation * FA_ENTRY_COUNT;
        output += 1;
    }
}


static void decimate_block(const void *read_block)
{
    int written = 0;
    for (int id = 0; id < FA_ENTRY_COUNT; id ++)
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

/* Area to write DD data. */
static struct decimated_data *dd_area;
/* Current offset into DD data area. */
unsigned int dd_offset;


/* Similar to decimate_column above, but condenses already decimated data by
 * further decimation.  In this case the algorithms are somewhat different. */
static void decimate_decimation(
    const struct decimated_data *input, struct decimated_data *output, int N)
{
    int64_t sumx = 0, sumy = 0;
    double sumvarx = 0, sumvary = 0;
    int32_t minx = INT32_MAX, maxx = INT32_MIN;
    int32_t miny = INT32_MAX, maxy = INT32_MIN;
    for (int i = 0; i < N; i ++, input ++)
    {
        sumx += input->meanx;
        sumy += input->meany;
        sumvarx = input->stdx * input->stdx;
        sumvary = input->stdy * input->stdy;
        if (input->minx < minx)     minx = input->minx;
        if (maxx < input->maxx)     maxx = input->maxx;
        if (input->miny < miny)     miny = input->miny;
        if (maxy < input->maxy)     maxy = input->maxy;
    }
    output->meanx = (int32_t) (sumx / N);
    output->meany = (int32_t) (sumy / N);
    output->minx = minx;
    output->maxx = maxx;
    output->miny = miny;
    output->maxy = maxy;
    output->stdx = (int32_t) round(sqrt(sumvarx / N));
    output->stdy = (int32_t) round(sqrt(sumvary / N));
}



/* In this case we work on decimated data sorted in the d_block and we write to
 * the in memory DD block. */
static void double_decimate_block(void)
{
    /* Note that we look backwards in time one second_decimation block to pick
     * up the data to be decimated here. */
    const struct decimated_data *input = d_block(0) - header->second_decimation;
    struct decimated_data *output = dd_area + dd_offset;
    int dd_block_size = header->dd_sample_count * header->major_block_count;

    int written = 0;
    for (int id = 0; id < FA_ENTRY_COUNT; id ++)
    {
        if (test_mask_bit(header->archive_mask, id))
        {
            decimate_decimation(input, output, header->second_decimation);
            input += header->d_sample_count;
            output += dd_block_size;
            written += 1;
        }
    }

    dd_offset = (dd_offset + 1) % (
        header->dd_sample_count * header->major_block_count);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level control. */


void process_block(const void *block)
{
    if (block)
    {
        transpose_block(block);
        decimate_block(block);
        bool must_write = advance_block();
        if (io_buffer.fa_offset % (
                header->first_decimation * header->second_decimation) == 0)
            double_decimate_block();
        if (must_write)
            write_major_block();
    }
    else
    {
        /* If we see a gap in the block then discard all the work we've done so
         * far. */
        reset_block();
        dd_offset = header->current_major_block * header->dd_sample_count;
    }
}


bool initialise_transform(
    int output_file,
    struct disk_header *header_, struct decimated_data *dd_area_)
{
    header = header_;
    dd_area = dd_area_;
    input_frame_count = header->input_block_size / FA_FRAME_SIZE;
    input_decimation_count = input_frame_count / header->first_decimation;
    dd_offset = header->current_major_block * header->dd_sample_count;

    initialise_io_buffer(output_file);
    return true;
}
