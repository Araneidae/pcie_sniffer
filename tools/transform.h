/* Header for data transposition and reduction functionality. */

struct disk_header;
struct data_index;


struct decimated_data {
    int32_t meanx, minx, maxx, stdx;
    int32_t meany, miny, maxy, stdy;
};


/* Processes a single input block by transposing and decimation.  If a major
 * block is filled then it is also written to disk. */
void process_block(const void *read_block, struct timespec *ts);


/* Interlocked access. */

/* Returns the current block index.  Note that data for the current block is
 * never valid! */
int get_block_index(void);
/* Returns selected index blocks. */
void get_index_blocks(int ix, int samples, struct data_index *result);
/* Reads dd data. */
void get_dd_data(
    int dd_index, int id, int samples, struct decimated_data *result);
/* Converts timestamp into corresponding index, or fails if timestamp is outside
 * the archive. */
bool timestamp_to_index(
    uint64_t timestamp, unsigned int *major_block, unsigned int *offset);

/* Returns an unlocked pointer to the header: should only be used to access the
 * constant header fields. */
const struct disk_header *get_header(void);


bool initialise_transform(
    struct disk_header *header, struct data_index *data_index,
    struct decimated_data *dd_area);
