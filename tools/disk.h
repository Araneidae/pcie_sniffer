/* Description of on-disk storage. */

/* Size of a single FA frame in bytes: 256 entries, each consisting of two 4
 * byte integers. */
#define FA_FRAME_SIZE   2048

/* A single page is allocated to the disk header. */
#define DISK_HEADER_SIZE    4096

/* Size of data block that we use for write locking, also used as validation
 * for data size: data area must be integer multiple of 1M. */
#define DATA_LOCK_BLOCK_SIZE    (1 << 20)

#define MAX_HEADER_BLOCKS \
    ((DISK_HEADER_SIZE - sizeof(struct disk_fields)) / \
        sizeof(struct block_record))

struct block_record {
    off64_t start_offset;   // Offset of first frame in block
    off64_t stop_offset;    // Offset of end of block (past last frame)
    uint64_t start_sec;     // Timestamp when write started
    uint64_t stop_sec;      // Timestamp when write stopped
};


/* The data is stored on disk in native format: it will be read and written
 * on the same machine, so the byte order in integers is not important. */
struct disk_header {
    struct disk_fields {
        char signature[7];          // Signature of valid disk block
        unsigned char version;      // Simple version number
        off64_t data_start;         // Offset into file of data area
        off64_t data_size;          // Size of data area
        uint32_t block_count;       // Number of active blocks
        uint32_t max_block_count;   // Set to MAX_BLOCKS
        uint32_t write_backlog;
        uint32_t write_buffer;
        uint32_t disk_status;
    } h;
    char __padding[DISK_HEADER_SIZE 
        - MAX_HEADER_BLOCKS * sizeof(struct block_record) 
        - sizeof(struct disk_fields)];
    struct block_record blocks[MAX_HEADER_BLOCKS];
} __attribute__((__packed__));


#define DISK_SIGNATURE      "FASNIFF"


/* Prepare a fresh disk header with the specified data area size. */
void initialise_header(struct disk_header *header, uint64_t data_size);
/* Reads the file size of the given file. */
bool get_filesize(int disk_fd, uint64_t *file_size);
/* Checks the given header for consistency. */
bool validate_header(struct disk_header *header, off64_t file_size);
/* Outputs header information in user friendly format. */
void print_header(FILE *out, struct disk_header *header);

/* Debug utility for dumping binary data in ASCII format. */
void dump_binary(FILE *out, void *buffer, size_t length);
