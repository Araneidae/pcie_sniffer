/* Interface to sniffer capture routines. */

struct fa_entry { int32_t x, y; };

/* Size of a single FA frame in bytes: 256 entries, each consisting of two 4
 * byte integers. */
#define FA_ENTRY_SIZE   (sizeof(struct fa_entry))
#define FA_ENTRY_COUNT  256
#define FA_FRAME_SIZE   (FA_ENTRY_COUNT * FA_ENTRY_SIZE)

bool initialise_sniffer(const char * device_name);

void terminate_sniffer(void);
