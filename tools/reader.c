/* Implements reading from disk. */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "sniffer.h"
#include "mask.h"
#include "reader.h"


enum read_action {
    READ_FA,
    READ_D,
    READ_DD
};

/* Result of parsing a read command. */
struct read_parse {
    filter_mask_t read_mask;        // List of BPMs to be read
    enum read_action read_action;   // Requested read action
    int samples;                    // Requested number of samples
    struct timespec start;          // Requested start time
};


/* */
static bool parse_request(char *request, struct read_parse *parse)
{
    return false;
}


void process_read(int scon, char *buf, ssize_t rx, size_t buf_size)
{
    struct read_parse parse;
    parse_request(buf, &parse);
}
