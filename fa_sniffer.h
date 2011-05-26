/* User space definitions for the fa_sniffer device driver. */

struct fa_entry { int32_t x, y; };

/* Each frame consists of 256 (X,Y) position pairs stored in sequence, making a
 * total of 2048 bytes for a single FA frame. */
#define FA_ENTRY_SIZE   (sizeof(struct fa_entry))
#define FA_ENTRY_COUNT  256
#define FA_FRAME_SIZE   (FA_ENTRY_COUNT * FA_ENTRY_SIZE)

/* Type for an entire row representing a single FA frame. */
struct fa_row { struct fa_entry row[FA_ENTRY_COUNT]; };


/* ioctl definitions. */

#define FASNIF_IOCTL_VERSION        1

/* Returns ioctl interface version number.  Just a sanity check. */
#define FASNIF_IOCTL_GET_VERSION    _IO('C', 0)
/* Restarts reading after eof on read(), subsequent calls to read() will succeed
 * if data is available. */
#define FASNIF_IOCTL_RESTART        _IO('C', 1)
/* Halts transfer if in progress.  Intended for debug use. */
#define FASNIF_IOCTL_HALT           _IO('C', 2)

/* Interrogates detailed status of FA sniffer. */
struct fa_status {
    unsigned int status;            // Hardware link status
    unsigned int partner;           // Associated link partner
    unsigned int last_interrupt;    // Status word from last interrupt
    unsigned int frame_errors;      // Hardware counts of communication errors
    unsigned int soft_errors;       //  accumulated since hardware initialised
    unsigned int hard_errors;
    bool running;                   // True if connection currently active
    bool overrun;                   // True if a buffer overrun occurred
};
#define FASNIF_IOCTL_GET_STATUS   _IOR('R', 1, struct fa_status)
