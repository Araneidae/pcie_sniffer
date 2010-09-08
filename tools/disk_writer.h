/* Interface to archive to disk writer. */

/* First stage of disk writer initialisation: opens the archive file and loads
 * the header into memory.  Can be called before initialising buffers. */
bool initialise_disk_writer(const char *file_name, uint32_t *input_block_size);
/* Starts writing files to disk.  Must be called after initialising the buffer
 * layer. */
bool start_disk_writer(void);
/* Orderly shutdown of the disk writer. */
void terminate_disk_writer(void);

/* Methods for access to writer thread. */

/* Asks the writer thread to write out the given block.  If a previously
 * requested write is still in progress then this blocks until the write has
 * completed. */
void schedule_write(off64_t offset, void *block, size_t length);

/* Requests permission to perform a read, blocks while an outstanding write is
 * in progress. */
void request_read(void);
