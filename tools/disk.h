/* Description of on-disk storage. */

/* A single page is allocated to the disk header. */
#define DISK_HEADER_SIZE    4096


/* Description of file store layout.
 *
 * Hierarchical description of store.  The header, index and DD data blocks are
 * held in memory, while the FA data blocks need to be kept on disk.
 *
 *  data_store = disk_header, index, DD_data, FA_data
 *  index = data_index[major_block_count]
 *  DD_data = DD_block[archive_mask_count]
 *  DD_block = decimated_data[DD_sample_count]
 *  FA_data = major_block[major_block_count]
 *  major_block = FA_block[archive_mask_count], D_block[archive_mask_count]
 *  FA_block = fa_entry[major_sample_count]
 *  D_block = decimated_data[D_sample_count]
 *
 * data_store
 * +-------------------------------------------
 * |disk_header
 * +-------+-----------------------------------
 * |index  |data_index
 * |       +-----------------------------------
 * |       *major_block_count
 * +-------+-------+---------------------------
 * |DD     |DD     |decimated_data
 * |data   |block  +---------------------------
 * |       |       *dd_total_count
 * |       +-----------------------------------
 * |       *archive_mask_count
 * +-------+-------+-------+-------------------
 * |FA     |major  |FA     |fa_entry
 * |data   |block  |block  +-------------------
 * |       |       |       *major_sample_count
 * |       |       +---------------------------
 * |       |       *archive_mask_count
 * |       |       +-------+-------------------
 * |       |       |D      |decimated_data
 * |       |       |block  +-------------------
 * |       |       |       *D_sample_count
 * |       |       +---------------------------
 * |       |       *archive_mask_count
 * |       +-----------------------------------
 * |       *major_block_count
 * +-------------------------------------------
 *
 * archive_mask_count = number of BPMs being archived, from 1 to 256
 * major_sample_count = parameter determined by output block size
 * major_block_count = paramter determined by file store size
 * D_sample_count = major_sample_count / first_decimation
 * DD_sample_count = D_sample_count / second_decimation
 * dd_total_count = dd_sample_count * major_block_count
 *
 * Note that major_sample_count must be a multiple of the two decimation factors
 * so that all indexing can be done in multiples of major blocks.  Thus the
 * index is by major block.
 */

/* The data is stored on disk in native format: it will be read and written
 * on the same machine, so the byte order in integers is not important. */
struct disk_header {
    char signature[7];          // Signature of valid disk block
    unsigned char version;      // Simple version number

    /* Description of data capture parameters. */
    filter_mask_t archive_mask; // List of BPM ids archived in this file
    uint32_t archive_mask_count; // Number of BPMs captured in this file
    uint32_t first_decimation;  // Decimation factors
    uint32_t second_decimation;
    uint32_t input_block_size;  // Controls read size from sniffer device

    /* Description of high level data structure.  The data offsets are a
     * multiple of page size and the data sizes are rounded up to a multiple
     * of page size to facilitate data transfer. */
    uint64_t index_data_start;  // Start of index block
    uint64_t dd_data_start;     // Start of double decimated data
    uint64_t major_data_start;  // Start of major data area
    uint32_t index_data_size;   // Size of index block
    uint32_t dd_data_size;      // Size of double decimated data area
    uint64_t total_data_size;   // Size of complete file, for check
    uint32_t dd_total_count;    // Total number of DD samples

    /* Parameters describing major data layout. */
    uint32_t major_block_count; // Total number of major blocks
    uint32_t major_block_size;  // Size of a major block in bytes
    uint32_t major_sample_count; // Samples in a major block
    uint32_t d_sample_count;    // Decimated samples in a major block
    uint32_t dd_sample_count;   // Double dec samples in a major block


    /* All the parameters above remain fixed during the operation of the
     * archiver, the parameters below are updated dynamically. */

    uint32_t current_major_block;   // This block is being written
    uint32_t last_duration;         // Time for last major block in microseconds
};


struct data_index {
    /* The major data blocks are indexed by their timestamp and we record the
     * duration of the block.  The duration is recorded in microseconds and for
     * ease of processing we record the timestamp in microseconds in the current
     * epoch. */
    uint64_t timestamp;
    uint32_t duration;
    /* Id 0 normally contains a cycle counter, so we also record the id for the
     * first read value. */
    uint32_t id_zero;
};


#define DISK_SIGNATURE      "FASNIFF"
#define DISK_VERSION        2


/* Two helper routines for converting sample number (within a major block) and
 * bpm id (as an index into the archive mask) into offsets into a major block
 * for both FA and D data. */
static inline int fa_data_offset(struct disk_header *header, int sample, int id)
{
    return FA_ENTRY_SIZE * (id * header->major_sample_count + sample);
}
static inline int d_data_offset(struct disk_header *header, int sample, int id)
{
    return
        FA_ENTRY_SIZE *
            header->major_sample_count * header->archive_mask_count +
        sizeof(struct decimated_data) * (id * header->d_sample_count + sample);
}


/* Prepare a fresh disk header with the specified parameters:
 *
 *  archive_mask
 *      Mask of BPMs to be captured.  This also determines how many samples will
 *      fit in the store.
 *  disk_size
 *      Total disk size.  This routine will fail if disk_size is too small.
 *  input_block_size
 *      Determines the capture size from the FA sniffer device.  Strictly
 *      speaking this doesn't determine the disk layout, but needs to be in a
 *      certain relationship with the disk layout.
 *  output_block_size
 *      This determines the block size for a single column of data from a single
 *      BPM, and thus determines the size of a major block.
 *  first_decimation
 *  second_decimation
 *      Data decimation factors.  These determine the data reduction factors for
 *      first and second stages of decimation.
 *
 * These parameters determine the layout and operation of the archiver. */
bool initialise_header(
    struct disk_header *header,
    filter_mask_t archive_mask,
    uint64_t disk_size,
    uint32_t input_block_size,
    uint32_t output_block_size,
    uint32_t first_decimation,
    uint32_t second_decimation,
    double sample_frequency);
/* Reads the file size of the given file. */
bool get_filesize(int disk_fd, uint64_t *file_size);
/* Checks the given header for consistency. */
bool validate_header(struct disk_header *header, uint64_t file_size);
/* Outputs header information in user friendly format. */
void print_header(FILE *out, struct disk_header *header);
/* Locks archive for exclusive access. */
bool lock_archive(int disk_fd);
