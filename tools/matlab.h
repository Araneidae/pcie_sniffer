/* Matlab support interface. */

bool write_matlab_header(
    int file_out, filter_mask_t filter_mask, unsigned int data_mask,
    unsigned int decimation, double timestamp, double frequency,
    unsigned int dump_length, const char *name, bool *squeeze);

unsigned int count_data_bits(unsigned int mask);

/* Converts a timestamp in FA sniffer format (microseconds in Unix epoch) to a
 * timestamp in matlab format (double days in Matlab epoch). */
double matlab_timestamp(uint64_t timestamp);
