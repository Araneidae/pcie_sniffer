/* Header for data transposition and reduction functionality. */

struct disk_header;
struct data_index;


struct decimated_data {
    struct fa_entry mean, min, max;
};


/* Processes a single input block by transposing and decimation.  If a major
 * block is filled then it is also written to disk. */
void process_block(const void *read_block, struct timespec *ts);


/* Interlocked access. */

/* Converts timestamp into corresponding index, or fails if timestamp is outside
 * the archive.  Returns number of available samples, the major block containing
 * the first data point, and the offset of the selected timestamp into that
 * block.  Also returns the true timestamp of the first sample, which may differ
 * from the requested timestamp. */
bool timestamp_to_index(
    uint64_t timestamp, uint64_t *samples_available,
    unsigned int *major_block, unsigned int *offset);

/* Searches a range of index blocks for a gap in the timestamp, returning true
 * iff a gap is found.  *start is updated to the index of the block directly
 * after the first gap and *blocks is decremented accordingly. */
bool find_gap(unsigned int *start, unsigned int *blocks);
const struct data_index * read_index(unsigned int ix);

/* Returns an unlocked pointer to the header: should only be used to access the
 * constant header fields. */
const struct disk_header *get_header(void);


bool initialise_transform(
    struct disk_header *header, struct data_index *data_index,
    struct decimated_data *dd_area);

// !!!!!!
// Not right.  Returns DD data area.
const struct decimated_data * get_dd_area(void);
