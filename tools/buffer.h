/* Interface to central memory buffer.
 *
 * At the heart of the archiver is a large memory buffer.  This is
 * continually filled from the FA archiver device all the time that the
 * communication controller network is running, and is emptied to disk
 * quickly enough that it should never overflow.
 *
 * There are a number of complications to this simple picture.
 *
 * Firstly, we support multiple readers of the buffer.  It is possible for
 * other applications to subscribe to the FA data stream, in which case they
 * will also be updated.  If a subscriber falls behind it is simply cut off!
 *
 * Secondly, we need some mechanism to cope with gaps in the FA data stream.
 * Whenever the communication controller network stops the feed of data into
 * the buffer is interrupted.  The presence of these gaps in the stream needs
 * to be recorded as they are written to disk.
 *
 * Finally, if writing to disk underruns it would be good to handle this
 * gracefully. */



/* Prepares central memory buffer. */
bool initialise_buffer(size_t block_size, size_t block_count);

/* Reserves the next slot in the buffer for writing. An entire contiguous
 * block of block_size bytes is returned, or NULL if the disk writer has
 * underrun and hasn't caught up yet -- in this case the writer needs to back
 * off and try again later. */
void * get_write_block(void);
/* Releases the previously reserved write block: only call if non-NULL value
 * returned by get_write_block(). */
void release_write_block(bool gap);


/* Creates a new reading connection to the buffer. */
struct reader_state * open_reader(bool reserved_reader);
/* Closes a previously opened reader connection. */
void close_reader(struct reader_state *reader);

/* Blocks until an entire block_size block is available to be read out,
 * returns pointer to data to be read.  If there is a gap in the available
 * data then NULL is returned, and release_write_block() should not be called
 * before calling get_read_block() again.
 *    If ts is not NULL then on a successful block read the timestamp of the
 * returned data is written to *ts. */
const void * get_read_block(
    struct reader_state *reader, int *backlog, struct timespec *ts);
/* Releases the write block.  If false is returned then the block was
 * overwritten while locked due to reader underrun; however, if the reader was
 * opened with reserved_reader set this is guaranteed not to happen.  Only
 * call if non-NULL value returned by get_read_block(). */
bool release_read_block(struct reader_state *reader);
/* Permanently halts the reader, interruping any waits in release_read_block()
 * and forcing further calls to get_read_block() to return NULL. */
void stop_reader(struct reader_state *reader);

/* Can be used to temporarily halt or resume buffered writing. */
void enable_buffer_write(bool enabled);


/* The block size (in bytes) used by the buffer is a global variable
 * initialised by initialise_buffer and left constant thereafter. */
extern size_t fa_block_size;
