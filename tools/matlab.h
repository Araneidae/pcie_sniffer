/* Matlab support interface. */

/* The matlab format symbol definitions we use.  Note that matlab is very buggy
 * when it comes to interpreting these formats, and only the following format
 * types are known to work in matlab arrays: miUINT8, miINT32, miDOUBLE.  In
 * particular miUINT32 definitely doesn't work properly! */
#define miINT8          1
#define miUINT8         2
#define miINT16         3
#define miUINT16        4
#define miINT32         5
#define miUINT32        6
#define miDOUBLE        9


void prepare_matlab_header(int32_t **hh, size_t buf_size);
int place_matrix_header(
    int32_t **hh, const char *name, int data_type,
    bool *squeeze, int data_length, int dimensions, ...);
void place_matlab_value(
    int32_t **hh, const char *name, int data_type, void *data);
void place_matlab_vector(
    int32_t **hh, const char *name, int data_type,
    void *data, int vector_length);

unsigned int count_data_bits(unsigned int mask);
int compute_mask_ids(uint8_t *array, filter_mask_t mask);

/* Converts a timestamp in FA sniffer format (microseconds in Unix epoch) to a
 * timestamp in matlab format (double days in Matlab epoch). */
double matlab_timestamp(uint64_t timestamp);
