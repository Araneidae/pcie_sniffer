/* Tool to use the fa_sniffer device to archive a continuous stream of FA
 * data.  Almost the same as running
 *      cat /dev/fa_sniffer0 >target-file
 * but that isn't reliable enough due to insufficient buffering. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "error.h"



#define K               1024
#define FA_FRAME_SIZE   (2 * K)
#define FA_BLOCK_SIZE   (64 * K)    // Block size for device read
#define WRITE_BLOCK_SIZE    (16 * K)


/* Bit mask of BPM ids, array of 256 bits. */
typedef uint32_t filter_mask_t[256/32];



/*****************************************************************************/
/*                               Static State                                */
/*****************************************************************************/


char * fa_sniffer_device = "/dev/fa_sniffer0";
char * output_file = "/scratch/test.out";
unsigned int buffer_size = 128 * K * K;    // 128M seems comfortable 


/* Frame buffer. */
char * frame_buffer;
size_t buffer_index_in = 0;
size_t buffer_index_out = 0;

filter_mask_t filter_mask;


bool reader_running = true;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t in_signal = PTHREAD_COND_INITIALIZER;

size_t max_size = 0;



/*****************************************************************************/
/*                             Support Routines                              */
/*****************************************************************************/


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Filter mask routines.                                                     */

void set_mask_bit(filter_mask_t mask, int bit)
{
    mask[bit >> 5] |= 1 << (bit & 0x1f);
}

bool test_mask_bit(filter_mask_t mask, int bit)
{
    return !!(mask[bit >> 5] & (1 << (bit & 0x1f)));
}

int count_mask_bits(filter_mask_t mask)
{
    int count = 0;
    for (int bit = 0; bit < 256; bit ++)
        if (test_mask_bit(mask, bit))
            count ++;
    return count;
}

void show_mask(filter_mask_t mask)
{
    for (int i = sizeof(filter_mask_t) / 4; i > 0; i --)
        printf("%08x", mask[i - 1]);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer access routines.                                                   */

size_t buffer_data_size(void)
{
    ssize_t delta = buffer_index_in - buffer_index_out;
    if (delta < 0)
        delta += buffer_size;
    return (size_t) delta;
}

void advance_index(size_t *index, size_t size)
{
    *index += size;
    if (*index >= buffer_size)
        *index -= buffer_size;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Thread locking routines.                                                  */

void lock(void)     { ASSERT_0(pthread_mutex_lock(&buffer_mutex)); }
void unlock(void)   { ASSERT_0(pthread_mutex_unlock(&buffer_mutex)); }
void signal(pthread_cond_t *cond) { ASSERT_0(pthread_cond_signal(cond)); }
void wait(pthread_cond_t *cond)
{
    ASSERT_0(pthread_cond_wait(cond, &buffer_mutex));
}



/*****************************************************************************/
/*                               Reader Thread                               */
/*****************************************************************************/

/* The reader thread must keep the device driver happy by reading everything
 * into its local buffer.  If the buffer overruns then we've lost and have to
 * start again, ditto if a read from the device fails. */



/* This thread simply reads from the FA sniffer device into the given buffer.
 * An error is indicated if the device exits. */
void * reader_thread(void *context)
{
    int fa_sniffer;
    if (TEST_IO_(
        fa_sniffer = open(fa_sniffer_device, O_RDONLY),
        "Can't open sniffer device %s", fa_sniffer_device))
    {
        ssize_t bytes_read;
        while (
            reader_running  &&
            TEST_IO_(
                bytes_read = read(fa_sniffer,
                    frame_buffer + buffer_index_in, FA_BLOCK_SIZE),
                "error reading sniffer, probable buffer overrun")  &&
            TEST_OK_(bytes_read == FA_BLOCK_SIZE,
                "unexpected short read: %d should be %d",
                bytes_read, FA_BLOCK_SIZE))
        {
//             printf("reading: % 9d, max: % 9d\r", size, max_size);
//             fflush(stdout);
            
            lock();
            advance_index(&buffer_index_in, FA_BLOCK_SIZE);
            /* Let the writer know that there's work to do. */
            signal(&in_signal);
            
            size_t size = buffer_data_size();
            if (size > max_size)  max_size = size;
            unlock();
            
            if (!TEST_OK_(size + FA_BLOCK_SIZE < buffer_size,
                    "buffer overrun"))
                /* Oops.  Out of room.  Could hope and pray that the writer
                 * will catch up before we run out of luck, but frankly that
                 * prospect is pretty bleak -- just give up. */
                break;
        }
        
        close(fa_sniffer);
    }
    return NULL;
}



/*****************************************************************************/
/*                               Writer Thread                               */
/*****************************************************************************/


bool write_block(int file, size_t size)
{
    /* Wait for enough data to arrive. */
    lock();
    while (buffer_data_size() < size)
        wait(&in_signal);
    unlock();

    /* Ensure the block we write fits entirely within the current buffer.  If
     * not, just write what we have. */
    if (buffer_index_out + size > buffer_size)
        size = buffer_size - buffer_index_out;

    ssize_t written;
    bool ok =
        TEST_IO_(
            written = write(file, frame_buffer + buffer_index_out, size),
            "Error writing output file")  &&
        written > 0;
    if (ok)
    {
        lock();
        advance_index(&buffer_index_out, written);
        unlock();
    }
    return ok;
}


void writer_thread(void)
{
    int output;
    if (TEST_IO_(
        output = open(output_file,
            O_WRONLY | O_TRUNC | O_CREAT | O_LARGEFILE, 0664),
        "Unable to open output file \"%s\"", output_file))
    {
        while (write_block(output, WRITE_BLOCK_SIZE))
            ;
        close(output);
    }
    reader_running = false;
}


/*****************************************************************************/
/*                               Option Parsing                              */
/*****************************************************************************/


void usage(char *argv0)
{
    printf(
"Usage: %s\n"
"Captures continuous FA streaming data to disk\n"
"\n"
"Options:\n"
"    -d:  Specify device to use for FA sniffer (default /dev/fa_sniffer0)\n"
"    -b:  Specify buffer size (default 128M bytes)\n"
"    -o:  Specify output file to write to\n"
"    -f:  Specify filter on BPM ids to record.  Can be a comma separated\n"
"         list of ids or ranges (two hyphen-separated ids).\n"
        , argv0);
}

bool read_uint(char *string, unsigned int *result)
{
    char *end;
    *result = strtoul(string, &end, 0);
    if (TEST_OK_(end > string, "nothing specified for option")) {
        switch (*end) {
            case 'K':   end++;  *result *= K;           break;
            case 'M':   end++;  *result *= K * K;       break;
            case '\0':  break;
        }
        return TEST_OK_(*end == '\0',
            "Unexpected characters in integer \"%s\"", string);
    } else
        return false;
}


bool read_id(char *original, char **string, int *id)
{
    char *next;
    *id = strtol(*string, &next, 0);
    return
        TEST_OK_(next > *string,
            "Number missing at \"%s\" (+%d)", original, *string - original)  &&
        DO_(*string = next)  &&
        TEST_OK_(0 <= *id  &&  *id < 256, "id %d out of range", *id);
}

bool read_filter(char *string, filter_mask_t mask)
{
    char *original = string;    // Just for error reporting
    memset(mask, 0, sizeof(filter_mask_t));
    
    bool ok = true;
    while (ok)
    {
        int id;
        ok = read_id(original, &string, &id);
        if (ok)
        {
            if (*string == '-')
            {
                string ++;
                int end;
                ok = read_id(original, &string, &end);
                for (int i = id; i <= end  &&  ok; i ++)
                    set_mask_bit(mask, i);
            }
            else
                set_mask_bit(mask, id);
        }
        
        if (*string != ',')
            break;
        string ++;
    }

    return
        ok  &&
        TEST_OK_(*string == '\0',
            "Unexpected characters at \"%s\" (+%d)",
            original, string - original);
}


bool process_options(int argc, char **argv)
{
    bool ok = true;
    while (ok) {
        switch (getopt(argc, argv, "+hd:b:o:f:")) {
            case 'h':   usage(argv[0]);                             exit(0);
            case 'd':   fa_sniffer_device = optarg;                 break;
            case 'b':   ok = read_uint(optarg, &buffer_size);       break;
            case 'o':   output_file = optarg;                       break;
            case 'f':   ok = read_filter(optarg, filter_mask);      break;
            default:
                fprintf(stderr, "Try `%s -h` for usage\n", argv[0]);
                return false;
            case -1:
                return true;
        }
    }
    return false;
}



/*****************************************************************************/
/*                            Startup and Control                            */
/*****************************************************************************/



void *logger_thread(void *context)
{
    while (true)
    {
        sleep(1);
        lock();
        size_t max = max_size;
        max_size = 0;
        unlock();
        
        printf("%d\n", max);
        fflush(stdout);
    }
}



void run_archiver(void)
{
    printf("mask, % 3d bits: ", count_mask_bits(filter_mask));
    show_mask(filter_mask);
    printf("\n");
    
    /* Convert buffer size into an integral multiple of FA_BLOCK_SIZE. */
    size_t block_count = buffer_size / FA_BLOCK_SIZE;
    buffer_size = block_count * FA_BLOCK_SIZE;
    if (TEST_OK_(block_count > 1, "buffer size is too small")  &&
        TEST_NULL_(
            frame_buffer = malloc(buffer_size),
            "Cannot allocate %u bytes for buffer", buffer_size))
    {
        pthread_t reader_id;
        ASSERT_0(pthread_create(&reader_id, NULL, reader_thread, NULL));

        pthread_t logger_id;
        ASSERT_0(pthread_create(&logger_id, NULL, logger_thread, NULL));
        writer_thread();
        
        void *value;
        ASSERT_0(pthread_join(reader_id, &value));
        free(frame_buffer);
    }
}


int main(int argc, char **argv)
{
    memset(filter_mask, 0xff, sizeof(filter_mask_t));
    if (process_options(argc, argv))
        run_archiver();
    return 0;
}
