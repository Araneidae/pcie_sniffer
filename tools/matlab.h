/* Matlab support interface. */

bool write_matlab_header(
    int file_out, filter_mask_t filter_mask, unsigned int data_mask,
    unsigned int dump_length, const char *name);

unsigned int count_data_bits(unsigned int mask);
