/* Description of on-disk storage. */

/* A single page is allocated to the disk header. */
#define DISK_HEADER_SIZE    4096

/* The number of header segments is automatically computed to fill the rest of
 * the disk header after the main content containing fields. */
#define MAX_HEADER_SEGMENTS \
    ((DISK_HEADER_SIZE - sizeof(struct disk_fields)) / \
        sizeof(struct segment_record))

struct segment_record {
    off64_t start_offset;   // Offset of first frame in segment
    off64_t stop_offset;    // Offset of end of segment (past last frame)
    uint64_t start_sec;     // Timestamp when write started
    uint64_t stop_sec;      // Timestamp when write stopped
};


/* The data is stored on disk in native format: it will be read and written
 * on the same machine, so the byte order in integers is not important. */
struct disk_header {
    struct disk_fields {
        char signature[7];          // Signature of valid disk block
        unsigned char version;      // Simple version number
        off64_t data_start;         // Offset into file of data area in bytes
        off64_t data_size;          // Size of data area in bytes
        uint32_t block_size;        // Size of single disk transfer
        uint32_t segment_count;     // Number of active segments
        uint32_t max_segment_count; // Set to MAX_HEADER_SEGMENTS

        /* The following fields are all obsolescent. */
        uint32_t write_backlog;
        uint32_t write_buffer;
        uint32_t disk_status;
    } h;
    char __padding[DISK_HEADER_SIZE
        - MAX_HEADER_SEGMENTS * sizeof(struct segment_record)
        - sizeof(struct disk_fields)];
    struct segment_record segments[MAX_HEADER_SEGMENTS];
} __attribute__((__packed__));


#define DISK_SIGNATURE      "FASNIFF"
#define DISK_VERSION        1


/* Prepare a fresh disk header with the specified data area size. */
void initialise_header(
    struct disk_header *header, uint32_t block_size, uint64_t data_size);
/* Reads the file size of the given file. */
bool get_filesize(int disk_fd, uint64_t *file_size);
/* Checks the given header for consistency. */
bool validate_header(struct disk_header *header, off64_t file_size);
/* Outputs header information in user friendly format. */
void print_header(FILE *out, struct disk_header *header);

/* Debug utility for dumping binary data in ASCII format. */
void dump_binary(FILE *out, void *buffer, size_t length);
