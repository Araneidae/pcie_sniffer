/* Implements reading from disk. */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "error.h"
#include "sniffer.h"
#include "mask.h"
#include "parse.h"
#include "transform.h"

#include "reader.h"


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
 *  read-request = "R" source mask "T" date-time "N" samples
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
    struct timespec ts;
    return
        parse_char(string, 'T')  &&
        parse_datetime(string, &ts)  &&
        DO_(*start = 1000000 * (uint64_t) ts.tv_sec + ts.tv_nsec / 1000);
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


static bool validate_read_request(struct read_parse *parse)
{
    return true;
}


/* Convert timestamp into block index. */
bool process_read(int scon, const char *buf)
{
    struct read_parse parse;
    char raw_mask[RAW_MASK_BYTES+1];
    int block, offset;
    return
        DO_PARSE("read request", parse_read_request, buf, &parse)  &&
        DO_(format_raw_mask(parse.read_mask, raw_mask))  &&
        validate_read_request(&parse)  &&
        DO_(printf("%s\n%u %llu %d\n",
            raw_mask, parse.samples, parse.start, parse.data_source))  &&
        timestamp_to_index(parse.start, &block, &offset)  &&
        DO_(printf("%d %d\n", block, offset));
}
