/* Header for data transposition and reduction functionality. */

struct decimated_data {
    int32_t meanx, minx, maxx, stdx;
    int32_t meany, miny, maxy, stdy;
};


/* Processes a single input block by transposing and decimation.  If a major
 * block is filled then it is also written to disk. */
void process_block(const void *read_block);


struct disk_header;
bool initialise_transform(
    struct disk_header *header, struct decimated_data *dd_area);
