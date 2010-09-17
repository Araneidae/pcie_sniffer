/* Matlab support interface. */

bool write_matlab_header(
    int file_out, filter_mask_t filter_mask, unsigned int data_mask,
    unsigned int decimation,
    unsigned int dump_length, const char *name, bool squeeze);

unsigned int count_data_bits(unsigned int mask);
